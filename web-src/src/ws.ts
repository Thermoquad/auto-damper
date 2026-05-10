import { createSignal, createEffect, onCleanup } from 'solid-js';

export type TemperatureMsg = { type: 'temperature'; celsius: number };
export type DamperMsg = {
  type: 'damper';
  mode: 'auto' | 'manual';
  angle: number;
  position: number | null;
};
export type HeaterMsg = {
  type: 'heater';
  power: string;
  step: string;
  mode: string;
  exhaust_temp: number;
  ambient_temp: number;
  voltage: number;
  target_temp: number;
  power_level: number;
  error: number;
  connected: boolean;
};
export type ResultMsg = { type: 'result'; ok: boolean; error?: string };
export type WsMessage = TemperatureMsg | DamperMsg | HeaterMsg | ResultMsg;

export function createWs() {
  const [connected, setConnected] = createSignal(false);
  const [temperature, setTemperature] = createSignal<TemperatureMsg | null>(null);
  const [damper, setDamper] = createSignal<DamperMsg | null>(null);
  const [heater, setHeater] = createSignal<HeaterMsg | null>(null);
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
        case 'temperature':
          setTemperature(msg);
          break;
        case 'damper':
          setDamper(msg);
          break;
        case 'heater':
          setHeater(msg);
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

  return { connected, temperature, damper, heater, lastResult, send };
}
