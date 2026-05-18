import { createSignal, createEffect, onCleanup } from 'solid-js';

export type DamperMsg = {
  type: 'damper';
  mode: 'auto' | 'manual' | 'heating' | 'cooling';
  route: 'inside' | 'outside';
  angle: number;
  inside_angle: number;
  outside_angle: number;
  core_threshold: number;
  cool_setpoint: number;
  cool_hysteresis: number;
  heater_name: string | null;
};
export type HeaterMsg = {
  type: 'heater';
  name: string | null;
  power: string;
  step: string;
  mode: string;
  core_temp: number;
  ambient_temp: number;
  voltage: number;
  target_temp: number;
  power_level: number;
  error: number;
  altitude_mode: boolean;
  startup_offset: number;
  shutdown_offset: number;
  connected: boolean;
};
export type HeaterDevice = { name: string; rssi: number; protocol: string };
export type HeatersMsg = { type: 'heaters'; connected: number; devices: HeaterDevice[] };
export type ResultMsg = { type: 'result'; ok: boolean; error?: string };
export type HeaterPendingMsg = {
  type: 'heater.pending';
  field: 'power' | 'mode' | 'altitude' | 'power_level' | 'auto_offsets';
  value?: boolean | string;
};

export type WsMessage = DamperMsg | HeaterMsg | HeatersMsg | ResultMsg | HeaterPendingMsg;

export type HeaterPending = {
  field: HeaterPendingMsg['field'];
  value?: boolean | string;
  /* Snapshot of the relevant telemetry value at the moment the
   * pending was armed. For commands that don't carry a target value
   * (altitude is a toggle, power_level is a delta), we clear early
   * when the next heater message shows a value different from this
   * snapshot. */
  before?: number | boolean | string;
  beforeShutdown?: number;
};

export function createWs() {
  const [connected, setConnected] = createSignal(false);
  const [damper, setDamper] = createSignal<DamperMsg | null>(null);
  const [heater, setHeater] = createSignal<HeaterMsg | null>(null);
  const [heaters, setHeaters] = createSignal<HeatersMsg | null>(null);
  const [lastResult, setLastResult] = createSignal<ResultMsg | null>(null);
  /* `pending` reflects a heater command that's been dispatched but
   * whose effect hasn't yet shown up in telemetry. Set on incoming
   * `heater.pending` (broadcast by firmware to every WS client),
   * cleared either when matching telemetry arrives or after a 3s
   * safety timeout. UI uses this to render a spinner on the button
   * being waited on, in sync across all connected clients. */
  const [pending, setPending] = createSignal<HeaterPending | null>(null);
  const PENDING_TIMEOUT_MS = 3000;
  let pendingTimeoutTimer: number | undefined;

  const armPending = (p: HeaterPending) => {
    if (pendingTimeoutTimer) clearTimeout(pendingTimeoutTimer);
    setPending(p);
    pendingTimeoutTimer = window.setTimeout(() => setPending(null), PENDING_TIMEOUT_MS);
  };
  const clearPending = () => {
    if (pendingTimeoutTimer) {
      clearTimeout(pendingTimeoutTimer);
      pendingTimeoutTimer = undefined;
    }
    setPending(null);
  };

  let ws: WebSocket | null = null;
  let reconnectTimer: number | undefined;
  /* Heartbeat: TCP doesn't notice a peer reboot for many seconds, so
   * onclose alone is a slow liveness signal. Send a cheap status query
   * every PING_MS and track time-since-last-message; if it crosses
   * WATCHDOG_MS, treat the socket as dead and force-reconnect. */
  const PING_MS = 3000;
  const WATCHDOG_MS = 7000;
  let pingTimer: number | undefined;
  let watchdogTimer: number | undefined;
  let lastMessageAt = 0;

  function clearHeartbeat() {
    if (pingTimer) { clearInterval(pingTimer); pingTimer = undefined; }
    if (watchdogTimer) { clearInterval(watchdogTimer); watchdogTimer = undefined; }
  }

  function startHeartbeat() {
    lastMessageAt = Date.now();
    pingTimer = window.setInterval(() => {
      if (ws?.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'damper.status' }));
      }
    }, PING_MS);
    watchdogTimer = window.setInterval(() => {
      if (Date.now() - lastMessageAt > WATCHDOG_MS) {
        clearHeartbeat();
        setConnected(false);
        ws?.close();
      }
    }, 1000);
  }

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${location.host}/api/ws`);

    ws.onopen = () => {
      setConnected(true);
      startHeartbeat();
    };

    ws.onmessage = (e) => {
      lastMessageAt = Date.now();
      const msg: WsMessage = JSON.parse(e.data);
      switch (msg.type) {
        case 'damper':
          setDamper(msg);
          break;
        case 'heater': {
          setHeater(msg);
          /* Early-clear pending when telemetry confirms the change. */
          const p = pending();
          if (p) {
            let done = false;
            if (p.field === 'power') {
              done = (p.value === true && msg.power === 'ON') ||
                     (p.value === false && msg.power === 'OFF');
            } else if (p.field === 'mode') {
              done = msg.mode === p.value;
            } else if (p.field === 'altitude') {
              done = msg.altitude_mode !== p.before;
            } else if (p.field === 'power_level') {
              done = msg.power_level !== p.before;
            } else if (p.field === 'auto_offsets') {
              done = msg.startup_offset !== p.before ||
                     msg.shutdown_offset !== p.beforeShutdown;
            }
            if (done) clearPending();
          }
          break;
        }
        case 'heaters':
          setHeaters(msg as HeatersMsg);
          break;
        case 'heater.pending': {
          const h = heater();
          armPending({
            field: msg.field,
            value: msg.value,
            before: msg.field === 'altitude' ? h?.altitude_mode :
                    msg.field === 'power_level' ? h?.power_level :
                    msg.field === 'auto_offsets' ? h?.startup_offset :
                    undefined,
            beforeShutdown: msg.field === 'auto_offsets' ? h?.shutdown_offset : undefined,
          });
          break;
        }
        case 'result':
          setLastResult(msg);
          break;
      }
    };

    ws.onclose = () => {
      clearHeartbeat();
      setConnected(false);
      reconnectTimer = window.setTimeout(connect, 2000);
    };

    ws.onerror = () => ws?.close();
  }

  function send(msg: Record<string, unknown>) {
    if (ws?.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(msg));
    }
  }

  connect();

  onCleanup(() => {
    clearHeartbeat();
    clearTimeout(reconnectTimer);
    ws?.close();
  });

  function sendCmd(msg: Record<string, unknown>) {
    setLastResult(null);
    send(msg);
  }

  return { connected, damper, heater, heaters, lastResult, pending, send, sendCmd };
}
