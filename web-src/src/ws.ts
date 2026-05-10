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
  connected: boolean;
};
export type HeaterDevice = { name: string; rssi: number; protocol: string };
export type HeatersMsg = { type: 'heaters'; connected: number; devices: HeaterDevice[] };
export type ResultMsg = { type: 'result'; ok: boolean; error?: string };

export type PositionEntry = { id: number; label: string; angle: number };
export type PositionsMsg = { type: 'positions'; positions: PositionEntry[] };

export type TargetEntry = { id: number; range: [number, number]; position: number };
export type TargetsMsg = { type: 'targets'; targets: TargetEntry[] };

export type WsMessage = TemperatureMsg | DamperMsg | HeaterMsg | HeatersMsg |
  ResultMsg | PositionsMsg | TargetsMsg;

export function createWs() {
  const [connected, setConnected] = createSignal(false);
  const [temperature, setTemperature] = createSignal<TemperatureMsg | null>(null);
  const [damper, setDamper] = createSignal<DamperMsg | null>(null);
  const [heater, setHeater] = createSignal<HeaterMsg | null>(null);
  const [heaters, setHeaters] = createSignal<HeatersMsg | null>(null);
  const [lastResult, setLastResult] = createSignal<ResultMsg | null>(null);
  const [positions, setPositions] = createSignal<PositionEntry[] | null>(null);
  const [targets, setTargets] = createSignal<TargetEntry[] | null>(null);

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
        case 'heaters':
          setHeaters(msg as HeatersMsg);
          break;
        case 'result':
          setLastResult(msg);
          break;
        case 'positions':
          setPositions((msg as PositionsMsg).positions);
          break;
        case 'targets':
          setTargets((msg as TargetsMsg).targets);
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

  return { connected, temperature, damper, heater, heaters, positions, setPositions, targets, setTargets, lastResult, send, sendCmd };
}
