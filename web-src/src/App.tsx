import { Show, For, createSignal, createEffect } from 'solid-js';
import '@voidable/ui';
import { createWs, type PositionsMsg, type TargetsMsg } from './ws';

export default function App() {
  const { connected, temperature, damper, heater, heaters, positions, setPositions, targets, setTargets, lastResult, send, sendCmd } = createWs();
  const [scanning, setScanning] = createSignal(false);
  const [sliding, setSliding] = createSignal(false);
  const [localAngle, setLocalAngle] = createSignal(0);
  const [configError, setConfigError] = createSignal<string | null>(null);
  const [damperTab, setDamperTab] = createSignal<'control' | 'ranges'>('control');
  let selectRef: HTMLElement | undefined;

  const selectedHeater = () => heater()?.connected ? heater()!.name ?? '' : '';

  createEffect(() => {
    const val = selectedHeater();
    if (selectRef) {
      const inner = selectRef.querySelector('select') as HTMLSelectElement | null;
      if (inner && inner.value !== val) {
        inner.value = val;
      }
    }
  });

  createEffect(() => {
    if (!sliding() && damper()) {
      setLocalAngle(damper()!.angle);
    }
  });

  const setAngle = (angle: number) => send({ type: 'damper.set', angle });
  const setAuto = () => send({ type: 'damper.set', auto: true });
  const scanHeaters = () => {
    setScanning(true);
    send({ type: 'heaters.scan', timeout: 5 });
    setTimeout(() => {
      send({ type: 'heaters.list' });
      setScanning(false);
    }, 5500);
  };
  const selectHeater = (name: string) => {
    if (!name) {
      send({ type: 'heaters.disconnect' });
      return;
    }
    if (heater()?.connected && heater()!.name === name) return;
    if (heater()?.connected) {
      send({ type: 'heaters.disconnect' });
      setTimeout(() => send({ type: 'heaters.connect', name }), 300);
    } else {
      send({ type: 'heaters.connect', name });
    }
  };
  const disconnectHeater = () => send({ type: 'heaters.disconnect' });
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
  const setTemp = (temp: number) => heaterCmd({ temp: Math.max(8, Math.min(36, temp)) });
  const adjustPower = (delta: number) => heaterCmd({ power_level: delta });

  const loadConfig = () => {
    send({ type: 'damper.status' });
    send({ type: 'positions.list' });
    send({ type: 'targets.list' });
  };
  createEffect(() => { if (connected()) loadConfig(); });

  const sendAndCheck = (msg: Record<string, unknown>, onError?: () => void) => {
    setConfigError(null);
    sendCmd(msg);
    setTimeout(() => {
      const r = lastResult();
      if (r && !r.ok) {
        setConfigError(r.error ?? 'unknown error');
        if (onError) onError();
      }
    }, 80);
  };

  const savePosition = (id: number, label: string, angle: number) => {
    const prev = positions() ?? [];
    setPositions(() => {
      const exists = prev.find(p => p.id === id);
      if (exists) return prev.map(p => p.id === id ? { ...p, label, angle } : p);
      return [...prev, { id, label, angle }];
    });
    sendAndCheck({ type: 'positions.set', id, label, angle }, () => setPositions(prev));
  };
  const deletePosition = (id: number) => {
    const prev = positions() ?? [];
    setPositions(prev.filter(p => p.id !== id));
    sendAndCheck({ type: 'positions.delete', id }, () => setPositions(prev));
  };
  const saveTarget = (id: number, range: [number, number], position: number) => {
    const prev = targets() ?? [];
    setTargets(() => {
      const exists = prev.find(t => t.id === id);
      if (exists) return prev.map(t => t.id === id ? { ...t, range, position } : t);
      return [...prev, { id, range, position }];
    });
    sendAndCheck({ type: 'targets.set', id, range, position }, () => setTargets(prev));
  };
  const deleteTarget = (id: number) => {
    const prev = targets() ?? [];
    setTargets(prev.filter(t => t.id !== id));
    sendAndCheck({ type: 'targets.delete', id }, () => setTargets(prev));
  };

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
            <span class="card-title">Duct Temperature</span>
          </div>
          <div class="card-body">
            <div class="stat-value">
              {temperature() ? `${temperature()!.celsius.toFixed(1)}°C` : '--'}
            </div>
          </div>
        </section>

        <section class="card">
          <div class="card-header">
            <span class="card-title">Damper</span>
            <Show when={damper()}>
              <void-badge color={damper()!.mode === 'auto' ? 'info' : 'notice'}>
                {damper()!.mode}
              </void-badge>
            </Show>
          </div>
          <div class="card-tabs">
            <button class={`tab ${damperTab() === 'control' ? 'active' : ''}`}
              onClick={() => setDamperTab('control')}>Control</button>
            <button class={`tab ${damperTab() === 'ranges' ? 'active' : ''}`}
              onClick={() => setDamperTab('ranges')}>Ranges</button>
          </div>
          <Show when={damperTab() === 'control'}>
            <div class="card-body">
              <Show when={damper()} fallback={<div class="stat-value">--</div>}>
                <div class="stat-value">{damper()!.angle.toFixed(1)}°</div>
                <Show when={damper()!.position !== null}>
                  <div class="stat-label">
                    {positions()?.find(p => p.id === damper()!.position)?.label ?? `Position ${damper()!.position}`}
                  </div>
                </Show>
              </Show>
            </div>
            <div class="card-actions damper-angle-controls">
              <div class="angle-slider-row">
                <input
                  type="range" min="0" max="270" step="0.5"
                  class="angle-slider"
                  value={localAngle()}
                  onPointerDown={() => setSliding(true)}
                  onInput={(e) => setLocalAngle(parseFloat(e.currentTarget.value))}
                  onChange={(e) => {
                    setAngle(parseFloat(e.currentTarget.value));
                    setSliding(false);
                  }}
                />
                <input
                  type="number" min="0" max="270" step="0.5"
                  class="angle-input"
                  value={localAngle().toFixed(1)}
                  onChange={(e) => {
                    const v = parseFloat(e.currentTarget.value);
                    if (!isNaN(v)) setAngle(Math.max(0, Math.min(270, v)));
                  }}
                />
              </div>
              <void-button variant="filled" size="sm" color="info" onClick={setAuto}>
                Auto
              </void-button>
            </div>
          </Show>
          <Show when={damperTab() === 'ranges'}>
            <div class="card-body config-body">
              <div class="config-section">
                <div class="config-section-header">
                  <span class="stat-label">Positions</span>
                  <void-button variant="outline" size="sm"
                    onClick={() => {
                      const ps = positions() ?? [];
                      const nextId = ps.length > 0 ? Math.max(...ps.map(p => p.id)) + 1 : 0;
                      savePosition(nextId, `Pos ${nextId}`, 0);
                    }}>
                    +
                  </void-button>
                </div>
                <Show when={positions()?.length} fallback={
                  <div class="config-empty">No positions configured</div>
                }>
                  <For each={positions()}>
                    {(pos) => (
                      <div class="config-row">
                        <void-button variant="outline" size="sm"
                          onClick={() => send({ type: 'damper.set', position: pos.id })}>
                          ▶
                        </void-button>
                        <input
                          type="text" class="config-label-input"
                          value={pos.label}
                          maxLength={15}
                          onChange={(e) => savePosition(pos.id, e.currentTarget.value, pos.angle)}
                        />
                        <input
                          type="number" class="config-angle-input"
                          min="0" max="270" step="0.5"
                          value={pos.angle.toFixed(1)}
                          onChange={(e) => {
                            const v = parseFloat(e.currentTarget.value);
                            if (!isNaN(v)) savePosition(pos.id, pos.label, Math.max(0, Math.min(270, v)));
                          }}
                        />
                        <span class="config-unit">°</span>
                        <void-button variant="outline" size="sm" color="error"
                          onClick={() => deletePosition(pos.id)}>
                          ×
                        </void-button>
                      </div>
                    )}
                  </For>
                </Show>
              </div>

              <div class="config-section">
                <div class="config-section-header">
                  <span class="stat-label">Targets</span>
                  <void-button variant="outline" size="sm"
                    onClick={() => {
                      const ts = targets() ?? [];
                      const ps = positions() ?? [];
                      if (!ps.length) return;
                      const nextId = ts.length > 0 ? Math.max(...ts.map(t => t.id)) + 1 : 0;
                      saveTarget(nextId, [0, 50], ps[0].id);
                    }}>
                    +
                  </void-button>
                </div>
                <Show when={configError()}>
                  <div class="config-error">{configError()}</div>
                </Show>
                <Show when={targets()?.length} fallback={
                  <div class="config-empty">No targets configured</div>
                }>
                  <For each={targets()}>
                    {(tgt) => (
                      <div class="config-row">
                        <input
                          type="number" class="config-temp-input"
                          step="1"
                          value={tgt.range[0]}
                          onChange={(e) => {
                            const v = parseFloat(e.currentTarget.value);
                            if (!isNaN(v)) saveTarget(tgt.id, [v, tgt.range[1]], tgt.position);
                          }}
                        />
                        <span class="config-separator">–</span>
                        <input
                          type="number" class="config-temp-input"
                          step="1"
                          value={tgt.range[1]}
                          onChange={(e) => {
                            const v = parseFloat(e.currentTarget.value);
                            if (!isNaN(v)) saveTarget(tgt.id, [tgt.range[0], v], tgt.position);
                          }}
                        />
                        <span class="config-unit">°C →</span>
                        <select class="config-pos-select"
                          value={tgt.position}
                          onChange={(e) => saveTarget(tgt.id, tgt.range, parseInt(e.currentTarget.value))}>
                          <For each={positions() ?? []}>
                            {(pos) => <option value={pos.id}>{pos.label}</option>}
                          </For>
                        </select>
                        <void-button variant="outline" size="sm" color="error"
                          onClick={() => deleteTarget(tgt.id)}>
                          ×
                        </void-button>
                      </div>
                    )}
                  </For>
                </Show>
              </div>
            </div>
          </Show>
        </section>

        <section class="card">
          <div class="card-header">
            <span class="card-title">Heater</span>
            <div class="card-header-right">
              <Show when={heaters()?.devices?.length}>
                <void-select size="sm"
                  ref={selectRef}
                  value={selectedHeater()}
                  placeholder="Select heater"
                  onChange={(e: Event) => selectHeater((e.target as HTMLSelectElement).value)}>
                  <option value="">None</option>
                  <For each={heaters()!.devices}>
                    {(device) => (
                      <option value={device.name}>
                        {device.name} ({device.protocol})
                      </option>
                    )}
                  </For>
                </void-select>
              </Show>
              <void-button variant="outline" size="sm" onClick={scanHeaters}
                disabled={scanning()}>
                {scanning() ? 'Scanning...' : 'Scan'}
              </void-button>
            </div>
          </div>
          <div class="card-body">
            <Show when={heater()?.connected} fallback={
              <div class="stat-value heater-placeholder">
                {heaters()?.devices?.length ? 'Select a heater' : 'Scan to discover heaters'}
              </div>
            }>
              <Show when={heater()!.error}>
                <div class="heater-error">
                  <void-badge color="error">E{heater()!.error}</void-badge>
                  <span>{heaterError(heater()!.error)}</span>
                </div>
              </Show>
              <div class="stat-grid">
                <div class="stat-item">
                  <div class="stat-label">Power</div>
                  <div class="stat-sm">{heater()!.power}</div>
                </div>
                <div class="stat-item">
                  <div class="stat-label">Step</div>
                  <div class="stat-sm">{heater()!.step}</div>
                </div>
                <div class="stat-item">
                  <div class="stat-label">Mode</div>
                  <div class="stat-sm">{heater()!.mode}</div>
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
                <div class="stat-item">
                  <div class="stat-label">Target</div>
                  <div class="stat-sm">{heater()!.target_temp}°C</div>
                </div>
                <div class="stat-item">
                  <div class="stat-label">Power Lvl</div>
                  <div class="stat-sm">{heater()!.power_level}</div>
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
                <div class="control-group">
                  <div class="stat-label">Mode</div>
                  <div class="control-row">
                    <void-button variant="outline" size="sm"
                      onClick={() => setMode('manual')}>
                      Manual
                    </void-button>
                    <void-button variant="outline" size="sm"
                      onClick={() => setMode('automatic')}>
                      Auto
                    </void-button>
                    <void-button variant="outline" size="sm"
                      onClick={() => setMode('fan')}>
                      Fan
                    </void-button>
                  </div>
                </div>
                <div class="control-group">
                  <div class="stat-label">Target Temp</div>
                  <div class="control-row">
                    <void-button variant="outline" size="sm"
                      onClick={() => setTemp(heater()!.target_temp - 1)}>
                      −
                    </void-button>
                    <span class="temp-display">{heater()!.target_temp}°C</span>
                    <void-button variant="outline" size="sm"
                      onClick={() => setTemp(heater()!.target_temp + 1)}>
                      +
                    </void-button>
                  </div>
                </div>
                <div class="control-group">
                  <div class="stat-label">Power Level</div>
                  <div class="control-row">
                    <void-button variant="outline" size="sm"
                      onClick={() => adjustPower(-1)}>
                      −
                    </void-button>
                    <span class="temp-display">{heater()!.power_level}</span>
                    <void-button variant="outline" size="sm"
                      onClick={() => adjustPower(1)}>
                      +
                    </void-button>
                  </div>
                </div>
              </div>
              <div class="card-actions">
                <void-button variant="outline" size="sm" color="error" onClick={disconnectHeater}>
                  Disconnect
                </void-button>
              </div>
            </Show>
          </div>
        </section>

      </main>
    </div>
  );
}
