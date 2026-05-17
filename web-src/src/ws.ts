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
  exhaust_temp: number;
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

export type WsMessage = DamperMsg | HeaterMsg | HeatersMsg | ResultMsg;

export function createWs() {
  const [connected, setConnected] = createSignal(false);
  const [damper, setDamper] = createSignal<DamperMsg | null>(null);
  const [heater, setHeater] = createSignal<HeaterMsg | null>(null);
  const [heaters, setHeaters] = createSignal<HeatersMsg | null>(null);
  const [lastResult, setLastResult] = createSignal<ResultMsg | null>(null);

  let ws: WebSocket | null = null;
  let reconnectTimer: number | undefined;

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${location.host}/api/ws`);

    ws.onopen = () => setConnected(true);

    ws.onmessage = (e) => {
      const msg: WsMessage = JSON.parse(e.data);
      switch (msg.type) {
        case 'damper':
          setDamper(msg);
          break;
        case 'heater':
          setHeater(msg);
          break;
        case 'heaters':
          setHeaters(msg as HeatersMsg);
          break;
        case 'result':
          setLastResult(msg);
          break;
      }
    };

    ws.onclose = () => {
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
    clearTimeout(reconnectTimer);
    ws?.close();
  });

  function sendCmd(msg: Record<string, unknown>) {
    setLastResult(null);
    send(msg);
  }

  return { connected, damper, heater, heaters, lastResult, send, sendCmd };
}
