import { createSignal, onCleanup } from 'solid-js';

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

export type HeaterState = {
  name: string;
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
  ble_rssi_dbm: number;
  connected: boolean;
};

export type HeaterStatesMsg = { type: 'heater_states'; heaters: HeaterState[] };

export type HeaterDevice = {
  name: string;
  rssi: number;
  protocol: string;
  connected: boolean;
};
export type HeatersMsg = { type: 'heaters'; devices: HeaterDevice[] };

export type ResultMsg = { type: 'result'; ok: boolean; error?: string };

export type HeaterPendingMsg = {
  type: 'heater.pending';
  target: string;
  field: 'power' | 'mode' | 'altitude' | 'power_level' | 'auto_offsets';
  value?: boolean | string;
};

export type WsMessage =
  | DamperMsg
  | HeaterStatesMsg
  | HeatersMsg
  | ResultMsg
  | HeaterPendingMsg;

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
  /* `heaters` is the per-name map of connected/managed heaters.
   * `devices` is the latest scan result list. */
  const [heaters, setHeaters] = createSignal<Record<string, HeaterState>>({});
  const [devices, setDevices] = createSignal<HeaterDevice[]>([]);
  const [lastResult, setLastResult] = createSignal<ResultMsg | null>(null);
  /* `pendingByName` reflects per-heater commands dispatched but not
   * yet confirmed by telemetry. Each heater can have its own pending
   * operation in flight, so the index is the heater's name. */
  const [pendingByName, setPendingByName] = createSignal<
    Record<string, HeaterPending>
  >({});
  const PENDING_TIMEOUT_MS = 3000;
  const pendingTimers: Record<string, number> = {};

  const armPending = (target: string, p: HeaterPending) => {
    const existing = pendingTimers[target];
    if (existing) clearTimeout(existing);
    setPendingByName(prev => ({ ...prev, [target]: p }));
    pendingTimers[target] = window.setTimeout(() => {
      delete pendingTimers[target];
      setPendingByName(prev => {
        const next = { ...prev };
        delete next[target];
        return next;
      });
    }, PENDING_TIMEOUT_MS);
  };

  const clearPending = (target: string) => {
    const t = pendingTimers[target];
    if (t) {
      clearTimeout(t);
      delete pendingTimers[target];
    }
    setPendingByName(prev => {
      if (!prev[target]) return prev;
      const next = { ...prev };
      delete next[target];
      return next;
    });
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

  function checkPendingResolved(h: HeaterState) {
    const p = pendingByName()[h.name];
    if (!p) return;
    let done = false;
    if (p.field === 'power') {
      done = (p.value === true && h.power === 'RUNNING') ||
             (p.value === true && h.power === 'STARTING') ||
             (p.value === false && h.power === 'OFF') ||
             (p.value === false && h.power === 'SHUTTING_DOWN');
    } else if (p.field === 'mode') {
      done = h.mode === p.value;
    } else if (p.field === 'altitude') {
      done = h.altitude_mode !== p.before;
    } else if (p.field === 'power_level') {
      done = h.power_level !== p.before;
    } else if (p.field === 'auto_offsets') {
      done = h.startup_offset !== p.before ||
             h.shutdown_offset !== p.beforeShutdown;
    }
    if (done) clearPending(h.name);
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
        case 'heater_states': {
          const map: Record<string, HeaterState> = {};
          for (const h of msg.heaters) {
            map[h.name] = h;
          }
          setHeaters(map);
          for (const h of msg.heaters) {
            checkPendingResolved(h);
          }
          /* Drop pending entries for heaters that disappeared. */
          for (const name of Object.keys(pendingByName())) {
            if (!map[name]) clearPending(name);
          }
          break;
        }
        case 'heaters':
          setDevices(msg.devices);
          break;
        case 'heater.pending': {
          const h = heaters()[msg.target];
          armPending(msg.target, {
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

  return {
    connected,
    damper,
    heaters,
    devices,
    lastResult,
    pendingByName,
    send,
    sendCmd,
  };
}
