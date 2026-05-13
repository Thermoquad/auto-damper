import { Show, createSignal, createEffect } from 'solid-js';
import '@voidable/ui';
import { createWs, type HeaterDevice } from './ws';

export default function App() {
  const { connected, damper, heater, heaters, lastResult, send, sendCmd } = createWs();
  const [sliding, setSliding] = createSignal(false);
  const [localAngle, setLocalAngle] = createSignal(0);
  const [damperTab, setDamperTab] = createSignal<'control' | 'config'>('control');
  const [selectedName, setSelectedName] = createSignal('');
  const [knownDevices, setKnownDevices] = createSignal<HeaterDevice[]>([]);
  let heaterSelectRef: HTMLElement | undefined;

  createEffect(() => {
    const hs = heaters();
    if (!hs?.devices?.length) return;
    setKnownDevices(prev => {
      const known = new Map(prev.map(d => [d.name, d]));
      for (const d of hs.devices) {
        known.set(d.name, d);
      }
      return Array.from(known.values());
    });
  });

  createEffect(() => {
    const h = heater();
    if (h?.connected && h.name) {
      setSelectedName(h.name);
    }
  });

  createEffect(() => {
    const devices = knownDevices();
    if (devices.length && !selectedName()) {
      setSelectedName(devices[0].name);
    }
  });

  createEffect(() => {
    const devices = knownDevices();
    const selected = selectedName();
    if (!heaterSelectRef) return;
    const el = heaterSelectRef as any;
    const sync = () => {
      const inner = el.querySelector('select') as HTMLSelectElement;
      if (!inner) return;
      inner.replaceChildren(...devices.map(d => {
        const opt = document.createElement('option');
        opt.value = d.name;
        opt.textContent = `${d.name} (${d.protocol})`;
        return opt;
      }));
      if (selected) inner.value = selected;
    };
    if (el.updateComplete) el.updateComplete.then(sync);
    else queueMicrotask(sync);
  });

  const heaterConnected = () => heater()?.connected && heater()!.name === selectedName();

  createEffect(() => {
    if (!sliding() && damper()) {
      setLocalAngle(damper()!.angle);
    }
  });

  let angleTimer: number | undefined;
  let pendingAngle: number | undefined;
  const setAngle = (angle: number) => {
    pendingAngle = angle;
    if (!angleTimer) {
      angleTimer = window.setInterval(() => {
        if (pendingAngle !== undefined) {
          send({ type: 'damper.set', angle: pendingAngle });
          pendingAngle = undefined;
        } else {
          clearInterval(angleTimer);
          angleTimer = undefined;
        }
      }, 50);
      send({ type: 'damper.set', angle });
      pendingAngle = undefined;
    }
  };
  const isAuto = () => damper()?.mode === 'auto';
  const toggleMode = () => {
    if (isAuto()) {
      send({ type: 'damper.set', angle: localAngle() });
    } else {
      send({ type: 'damper.set', auto: true });
    }
  };
  const heaterCmd = (cmd: Record<string, unknown>) =>
    send({ type: 'heater.command', ...cmd });
  const heaterError = (code: number): string => {
    const errors: Record<number, string> = {
      2: 'Voltage fault', 3: 'Glow plug fault', 4: 'Fuel pump fault',
      5: 'High temperature', 6: 'Fan/motor fault', 7: 'Communication fault',
      8: 'Flame-out', 9: 'Temp sensor fault', 10: 'Failed to start',
      11: 'CO alarm',
    };
    return errors[code] ?? `Fault ${code}`;
  };
  const setPower = (on: boolean) => heaterCmd({ power: on });
  const setMode = (mode: string) => heaterCmd({ mode });
  const adjustPower = (delta: number) => heaterCmd({ power_level: delta });
  const toggleAltitude = () => heaterCmd({ altitude: true });
  const setAutoOffsets = (startup: number, shutdown: number) =>
    heaterCmd({ startup_offset: startup, shutdown_offset: shutdown });
  const adjustOffset = (field: 'startup' | 'shutdown', delta: number) => {
    const h = heater()!;
    const cur = field === 'startup' ? h.startup_offset : h.shutdown_offset;
    const next = Math.max(3, Math.min(10, cur + delta));
    if (field === 'startup') setAutoOffsets(next, h.shutdown_offset);
    else setAutoOffsets(h.startup_offset, next);
  };

  const saveConfig = (field: string, value: number) => {
    sendCmd({ type: 'damper.config', [field]: value });
  };
  const setDamperHeater = (name: string) => {
    sendCmd({ type: 'damper.heater', name });
  };

  const loadConfig = () => {
    send({ type: 'damper.status' });
    send({ type: 'heaters.list' });
    send({ type: 'heater.status' });
  };
  createEffect(() => { if (connected()) loadConfig(); });

  return (
    <div class="shell">
      <nav class="nav">
        <div class="nav-inner">
          <span class="nav-wordmark">auto-damper</span>
          <div class="nav-right">
            <void-badge color={connected() ? 'success' : 'error'}>
              {connected() ? 'Connected' : 'Disconnected'}
            </void-badge>
          </div>
        </div>
      </nav>

      <main class="main">
        <section class="card">
          <div class="card-header">
            <span class="card-title">Damper</span>
            <Show when={damper()}>
              <div class="card-header-right">
                <void-badge color={damper()!.route === 'inside' ? 'success' : 'default'}>
                  {damper()!.route}
                </void-badge>
              </div>
            </Show>
          </div>
          <div class="card-tabs">
            <button class={`tab ${damperTab() === 'control' ? 'active' : ''}`}
              onClick={() => setDamperTab('control')}>Control</button>
            <button class={`tab ${damperTab() === 'config' ? 'active' : ''}`}
              onClick={() => setDamperTab('config')}>Config</button>
          </div>
          <Show when={damperTab() === 'control'}>
            <div class="card-body">
              <Show when={damper()} fallback={<div class="stat-value">--</div>}>
                <div class="stat-value">{damper()!.angle.toFixed(1)}°</div>
              </Show>
            </div>
            <div class="card-actions damper-angle-controls">
              <div class="angle-slider-row">
                <input
                  type="range" min="0" max="270" step="0.5"
                  class="angle-slider"
                  disabled={isAuto()}
                  value={localAngle()}
                  onPointerDown={() => setSliding(true)}
                  onInput={(e) => {
                    const v = parseFloat(e.currentTarget.value);
                    setLocalAngle(v);
                    setAngle(v);
                  }}
                  onChange={() => {
                    setSliding(false);
                  }}
                />
                <input
                  type="number" min="0" max="270" step="0.5"
                  class="angle-input"
                  disabled={isAuto()}
                  value={localAngle().toFixed(1)}
                  onChange={(e) => {
                    const v = parseFloat(e.currentTarget.value);
                    if (!isNaN(v)) setAngle(Math.max(0, Math.min(270, v)));
                  }}
                />
              </div>
              <div class="mode-toggle-row">
                <void-button
                  variant="filled" size="sm"
                  color={isAuto() ? 'info' : 'notice'}
                  onClick={toggleMode}>
                  {isAuto() ? 'Auto' : 'Manual'}
                </void-button>
              </div>
            </div>
          </Show>
          <Show when={damperTab() === 'config'}>
            <div class="card-body config-body">
              <div class="config-section">
                <span class="stat-label">Angles</span>
                <div class="config-row">
                  <span class="config-unit">Inside</span>
                  <input
                    type="number" class="config-angle-input"
                    min="0" max="270" step="0.5"
                    value={damper()?.inside_angle?.toFixed(1) ?? '0.0'}
                    onChange={(e) => {
                      const v = parseFloat(e.currentTarget.value);
                      if (!isNaN(v)) saveConfig('inside_angle', Math.max(0, Math.min(270, v)));
                    }}
                  />
                  <span class="config-unit">°</span>
                </div>
                <div class="config-row">
                  <span class="config-unit">Outside</span>
                  <input
                    type="number" class="config-angle-input"
                    min="0" max="270" step="0.5"
                    value={damper()?.outside_angle?.toFixed(1) ?? '270.0'}
                    onChange={(e) => {
                      const v = parseFloat(e.currentTarget.value);
                      if (!isNaN(v)) saveConfig('outside_angle', Math.max(0, Math.min(270, v)));
                    }}
                  />
                  <span class="config-unit">°</span>
                </div>
              </div>
              <div class="config-section">
                <span class="stat-label">Auto Routing</span>
                <div class="config-row">
                  <span class="config-unit">Core Threshold</span>
                  <input
                    type="number" class="config-temp-input"
                    min="0" max="500" step="5"
                    value={damper()?.core_threshold?.toFixed(0) ?? '150'}
                    onChange={(e) => {
                      const v = parseFloat(e.currentTarget.value);
                      if (!isNaN(v)) saveConfig('core_threshold', v);
                    }}
                  />
                  <span class="config-unit">°C</span>
                </div>
                <div class="config-row">
                  <span class="config-unit">Heater ID</span>
                  <Show when={knownDevices().length} fallback={
                    <span class="config-empty">{damper()?.heater_name ?? 'none'}</span>
                  }>
                    <select class="config-pos-select"
                      value={damper()?.heater_name ?? ''}
                      onChange={(e) => setDamperHeater(e.currentTarget.value)}>
                      <option value="">None</option>
                      {knownDevices().map(d => (
                        <option value={d.name}>{d.name}</option>
                      ))}
                    </select>
                  </Show>
                </div>
              </div>
            </div>
          </Show>
        </section>

        <section class="card">
          <div class="card-header">
            <span class="card-title">Heater</span>
            <Show when={knownDevices().length}>
              <void-select size="sm"
                ref={heaterSelectRef}
                onChange={(e: Event) => setSelectedName((e.target as HTMLSelectElement).value)}
              />
            </Show>
          </div>
          <div class="card-body">
            <Show when={selectedName()} fallback={
              <div class="stat-value heater-placeholder">
                Searching for heaters...
              </div>
            }>
              <Show when={heaterConnected() && heater()!.error}>
                <div class="heater-error">
                  <void-badge color="error">E{heater()!.error}</void-badge>
                  <span>{heaterError(heater()!.error)}</span>
                </div>
              </Show>
              <Show when={heaterConnected()} fallback={
                <div class="heater-disconnected">
                  <void-badge color="error">Disconnected</void-badge>
                  <span>{selectedName()}</span>
                </div>
              }>
                <div class="stat-grid">
                  <div class="stat-item">
                    <div class="stat-label">Power</div>
                    <div class="stat-sm">{heater()!.power}</div>
                  </div>
                  <div class="stat-item">
                    <div class="stat-label">State</div>
                    <div class="stat-sm">{heater()!.step}</div>
                  </div>
                  <div class="stat-item">
                    <div class="stat-label">Core</div>
                    <div class="stat-sm">{heater()!.exhaust_temp.toFixed(1)}°C</div>
                  </div>
                  <div class="stat-item">
                    <div class="stat-label">Ambient</div>
                    <div class="stat-sm">{heater()!.ambient_temp.toFixed(1)}°C</div>
                  </div>
                  <div class="stat-item">
                    <div class="stat-label">Voltage</div>
                    <div class="stat-sm">{heater()!.voltage.toFixed(1)}V</div>
                  </div>
                </div>
                <div class="heater-controls-bar">
                  <div class="control-group">
                    <div class="stat-label">Power</div>
                    <div class="control-row">
                      <void-button variant={heater()!.power === 'OFF' ? 'outline' : 'filled'}
                        size="sm" color="success"
                        onClick={() => setPower(true)}>
                        On
                      </void-button>
                      <void-button variant={heater()!.power === 'OFF' ? 'filled' : 'outline'}
                        size="sm" color="error"
                        onClick={() => setPower(false)}>
                        Off
                      </void-button>
                    </div>
                  </div>
                </div>
              </Show>
            </Show>
          </div>
          <Show when={heaterConnected()}>
            <div class="card-tabs">
              <button class={`tab ${heater()!.mode === 'manual' ? 'active' : ''}`}
                onClick={() => setMode('manual')}>Manual</button>
              <button class={`tab ${heater()!.mode === 'automatic' ? 'active' : ''}`}
                onClick={() => setMode('automatic')}>Auto</button>
              <button class={`tab ${heater()!.mode === 'fan' ? 'active' : ''}`}
                onClick={() => setMode('fan')}>Fan</button>
            </div>
            <div class="heater-mode-controls">
              <div class="control-group">
                <div class="stat-label">
                  {heater()!.mode === 'automatic' ? 'Target Temp' : 'Power Level'}
                </div>
                <div class="control-row">
                  <void-button variant="outline" size="sm"
                    onClick={() => adjustPower(-1)}>
                    −
                  </void-button>
                  <span class="temp-display">
                    {heater()!.power_level}{heater()!.mode === 'automatic' ? '°C' : ''}
                  </span>
                  <void-button variant="outline" size="sm"
                    onClick={() => adjustPower(1)}>
                    +
                  </void-button>
                </div>
              </div>
              <Show when={heater()!.mode === 'automatic'}>
                <div class="control-group">
                  <div class="stat-label">Min Temp</div>
                  <div class="control-row">
                    <void-button variant="outline" size="sm"
                      onClick={() => adjustOffset('startup', -1)}>
                      −
                    </void-button>
                    <span class="temp-display">
                      {heater()!.startup_offset}°C
                    </span>
                    <void-button variant="outline" size="sm"
                      onClick={() => adjustOffset('startup', 1)}>
                      +
                    </void-button>
                  </div>
                </div>
                <div class="control-group">
                  <div class="stat-label">Max Temp</div>
                  <div class="control-row">
                    <void-button variant="outline" size="sm"
                      onClick={() => adjustOffset('shutdown', -1)}>
                      −
                    </void-button>
                    <span class="temp-display">
                      {heater()!.shutdown_offset}°C
                    </span>
                    <void-button variant="outline" size="sm"
                      onClick={() => adjustOffset('shutdown', 1)}>
                      +
                    </void-button>
                  </div>
                </div>
              </Show>
              <Show when={heater()!.mode !== 'fan'}>
                <div class="control-group">
                  <div class="stat-label">Altitude</div>
                  <div class="control-row">
                    <void-button
                      variant={heater()!.altitude_mode ? 'filled' : 'outline'}
                      size="sm"
                      color={heater()!.altitude_mode ? 'warning' : 'default'}
                      onClick={toggleAltitude}>
                      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                        <path d="M3 20h18l-6.921 -14.612a2.3 2.3 0 0 0 -4.158 0l-6.921 14.612z" />
                        <path d="M7.5 11l2 2.5l2.5 -2.5l2 3l2.5 -2" />
                      </svg>
                    </void-button>
                  </div>
                </div>
              </Show>
            </div>
          </Show>
        </section>

      </main>
    </div>
  );
}
