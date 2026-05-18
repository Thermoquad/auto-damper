import { Show, For, createSignal, createEffect } from 'solid-js';
import '@voidable/ui';
import { createWs, type HeaterDevice } from './ws';

export default function App() {
  const { connected, damper, heater, heaters, lastResult, send, sendCmd } = createWs();
  const [sliding, setSliding] = createSignal(false);
  const [localAngle, setLocalAngle] = createSignal(0);
  const [damperTab, setDamperTab] = createSignal<'control' | 'config'>('control');
  const [selectedName, setSelectedName] = createSignal('');
  const [knownDevices, setKnownDevices] = createSignal<HeaterDevice[]>([]);
  /** Manual-mode position limiter: when on, clamp angle to the
   *  [inside_angle, outside_angle] window. Browser-local state. */
  const [limitToRoute, setLimitToRoute] = createSignal(true);
  const manualMin = () => {
    if (!limitToRoute()) return 0;
    const d = damper();
    return d ? Math.min(d.inside_angle, d.outside_angle) : 0;
  };
  const manualMax = () => {
    if (!limitToRoute()) return 270;
    const d = damper();
    return d ? Math.max(d.inside_angle, d.outside_angle) : 270;
  };
  let configHeaterSelectRef: HTMLElement | undefined;

  /** void-select captures children once on connect, then moves them into
   *  its rendered native <select>. We sync options via the inner element
   *  so reactive changes propagate without remounting. */
  const syncVoidSelect = (el: HTMLElement, options: Array<{value: string; label: string}>, selected: string) => {
    const sync = () => {
      const inner = el.querySelector('select') as HTMLSelectElement | null;
      if (!inner) return;
      inner.replaceChildren(...options.map(o => {
        const opt = document.createElement('option');
        opt.value = o.value;
        opt.textContent = o.label;
        return opt;
      }));
      inner.value = selected;
    };
    const updateComplete = (el as any).updateComplete;
    if (updateComplete) updateComplete.then(sync);
    else queueMicrotask(sync);
  };

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
    const selected = damper()?.heater_name ?? '';
    if (!configHeaterSelectRef) return;
    syncVoidSelect(
      configHeaterSelectRef,
      [{value: '', label: 'None'}, ...devices.map(d => ({value: d.name, label: d.name}))],
      selected,
    );
  });

  const heaterConnected = () => heater()?.connected && heater()!.name === selectedName();

  createEffect(() => {
    if (!sliding() && damper()) {
      setLocalAngle(damper()!.angle);
    }
  });

  /* Rate-limit servo commands to 10/sec (100 ms) so the firmware's
   * WebSocket / TCP layer isn't flooded while the user is sliding. The
   * first move is sent immediately; subsequent calls within the window
   * collapse to the most recent value and ship on the next tick. */
  const SERVO_CMD_INTERVAL_MS = 100;
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
      }, SERVO_CMD_INTERVAL_MS);
      send({ type: 'damper.set', angle });
      pendingAngle = undefined;
    }
  };
  const isManual = () => damper()?.mode === 'manual';
  const setDamperMode = (mode: string) => {
    if (mode === 'manual') {
      send({ type: 'damper.set', angle: localAngle() });
    } else {
      send({ type: 'damper.set', mode });
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
          <void-tabs
            value={damperTab()}
            size="lg"
            on:void-change={(e: CustomEvent<{value: string}>) => {
              /* void-change bubbles from every nested void-* component
                 (slider, toggle-group, number-input, select). Only act on
                 our own dispatch. */
              if ((e.target as HTMLElement).tagName !== 'VOID-TABS') return;
              setDamperTab(e.detail.value as 'control' | 'config');
            }}>
            <void-tab-panel tab="control" label="Control">
              <div class="card-body">
                <Show when={damper()} fallback={<div class="stat-value">--</div>}>
                  <div class="stat-value">{damper()!.angle.toFixed(1)}°</div>
                </Show>
              </div>
              <div class="card-actions damper-angle-controls">
                <void-slider
                  size="lg"
                  min={manualMin()} max={manualMax()} step={0.5}
                  disabled={!isManual() || undefined}
                  value={localAngle()}
                  on:void-input={(e: CustomEvent<{value: number}>) => {
                    setSliding(true);
                    setLocalAngle(e.detail.value);
                    setAngle(e.detail.value);
                  }}
                  on:void-change={(e: CustomEvent<{value: number}>) => {
                    setSliding(false);
                    setLocalAngle(e.detail.value);
                    setAngle(e.detail.value);
                  }}
                />
                <void-number-input
                  controls="sides"
                  size="lg"
                  min={manualMin()} max={manualMax()} step={0.5} precision={1}
                  disabled={!isManual() || undefined}
                  value={localAngle()}
                  on:void-change={(e: CustomEvent<{value: number}>) =>
                    setAngle(e.detail.value)}
                />
                <void-toggle-group
                  value={damper()?.mode ?? 'auto'}
                  size="lg"
                  on:void-change={(e: CustomEvent<{value: string}>) => {
                    if (e.detail.value) setDamperMode(e.detail.value);
                  }}>
                  <void-toggle value="auto">Auto</void-toggle>
                  <void-toggle value="heating">Heating</void-toggle>
                  <void-toggle value="cooling">Cooling</void-toggle>
                  <void-toggle value="manual">Manual</void-toggle>
                </void-toggle-group>
                <Show when={isManual()}>
                  <label class="limit-switch">
                    <void-switch
                      size="xxl"
                      checked={limitToRoute() || undefined}
                      on:void-change={(e: CustomEvent<{checked: boolean}>) =>
                        setLimitToRoute(e.detail.checked)}
                    />
                    <span>Limit to route range</span>
                  </label>
                </Show>
              </div>
            </void-tab-panel>
            <void-tab-panel tab="config" label="Config">
              <div class="damper-config">
                <div class="control-group">
                  <div class="stat-label">Inside Angle</div>
                  <void-number-input
                    controls="sides"
                    size="lg"
                    min={0} max={270} step={0.5} precision={1}
                    value={damper()?.inside_angle ?? 0}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      saveConfig('inside_angle', e.detail.value)}
                  />
                </div>
                <div class="control-group">
                  <div class="stat-label">Outside Angle</div>
                  <void-number-input
                    controls="sides"
                    size="lg"
                    min={0} max={270} step={0.5} precision={1}
                    value={damper()?.outside_angle ?? 270}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      saveConfig('outside_angle', e.detail.value)}
                  />
                </div>
                <div class="control-group">
                  <div class="stat-label">Core Threshold</div>
                  <void-number-input
                    controls="sides"
                    size="lg"
                    min={0} max={500} step={5} precision={0}
                    value={damper()?.core_threshold ?? 150}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      saveConfig('core_threshold', e.detail.value)}
                  />
                </div>
                <div class="control-group">
                  <div class="stat-label">Heater ID</div>
                  <Show when={knownDevices().length} fallback={
                    <span class="config-empty">{damper()?.heater_name ?? 'none'}</span>
                  }>
                    <void-select
                      size="lg"
                      ref={configHeaterSelectRef}
                      on:void-change={(e: CustomEvent<{value: string}>) =>
                        setDamperHeater(e.detail.value)}
                    />
                  </Show>
                </div>
                <div class="control-group">
                  <div class="stat-label">Cool Setpoint</div>
                  <void-number-input
                    controls="sides"
                    size="lg"
                    min={0} max={50} step={1} precision={0}
                    value={damper()?.cool_setpoint ?? 25}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      saveConfig('cool_setpoint', e.detail.value)}
                  />
                </div>
                <div class="control-group">
                  <div class="stat-label">Cool Hysteresis</div>
                  <void-number-input
                    controls="sides"
                    size="lg"
                    min={0} max={20} step={1} precision={0}
                    value={damper()?.cool_hysteresis ?? 4}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      saveConfig('cool_hysteresis', e.detail.value)}
                  />
                </div>
              </div>
            </void-tab-panel>
          </void-tabs>
        </section>

        <section class="card">
          <div class="card-header">
            <span class="card-title">Heater</span>
            <Show when={knownDevices().length ? knownDevices() : null} keyed>
              {devices => (
                <void-combobox
                  size="lg"
                  placeholder="Select heater"
                  value={selectedName()}
                  on:void-change={(e: CustomEvent<{value: string}>) =>
                    setSelectedName(e.detail.value)}>
                  <For each={devices}>
                    {d => <void-option value={d.name}>{d.name} ({d.protocol})</void-option>}
                  </For>
                </void-combobox>
              )}
            </Show>
          </div>
          <div class="card-body">
            <Show when={selectedName()} fallback={
              <div class="stat-value heater-placeholder">
                Searching for heaters...
              </div>
            }>
              <Show when={heaterConnected() && heater()!.error}>
                <void-alert color="error" variant="subtle">
                  E{heater()!.error} — {heaterError(heater()!.error)}
                </void-alert>
              </Show>
              <Show when={heaterConnected()} fallback={
                <void-alert color="default" variant="subtle">
                  <void-badge color="error">Disconnected</void-badge>
                  <span> {selectedName()}</span>
                </void-alert>
              }>
                <void-collapsible heading="Status">
                  <div class="stat-grid">
                    <void-stat size="sm" label="System" value={heater()!.power} />
                    <void-stat size="sm" label="State" value={heater()!.step} />
                    <void-stat size="sm" label="Core" value={`${heater()!.core_temp.toFixed(1)}°C`} />
                    <void-stat size="sm" label="Ambient" value={`${heater()!.ambient_temp.toFixed(1)}°C`} />
                    <void-stat size="sm" label="Voltage" value={`${heater()!.voltage.toFixed(1)}V`} />
                  </div>
                </void-collapsible>
              </Show>
            </Show>
          </div>
          <Show when={heaterConnected()}>
            <div class="heater-controls">
              <div class="control-group">
                <div class="stat-label">Power</div>
                <div class="control-row">
                  <void-button variant={heater()!.power === 'OFF' ? 'outline' : 'filled'}
                    size="lg" color="success"
                    onClick={() => setPower(true)}>On</void-button>
                  <void-button variant={heater()!.power === 'OFF' ? 'filled' : 'outline'}
                    size="lg" color="error"
                    onClick={() => setPower(false)}>Off</void-button>
                </div>
              </div>
              <div class="control-group">
                <div class="stat-label">Mode</div>
                <void-toggle-group
                  value={heater()!.mode}
                  size="lg"
                  on:void-change={(e: CustomEvent<{value: string}>) => {
                    if (e.detail.value) setMode(e.detail.value);
                  }}>
                  <void-toggle value="manual">Manual</void-toggle>
                  <void-toggle value="automatic">Auto</void-toggle>
                  <void-toggle value="fan">Fan</void-toggle>
                </void-toggle-group>
              </div>
              <div class="control-group">
                <div class="stat-label">
                  {heater()!.mode === 'automatic' ? 'Target Temp' : 'Power Level'}
                </div>
                <void-number-input
                  controls="sides"
                  size="lg"
                  min={heater()!.mode === 'automatic' ? 8 : 1}
                  max={heater()!.mode === 'automatic' ? 36 : 10}
                  step={1} precision={0}
                  value={heater()!.power_level}
                  on:void-change={(e: CustomEvent<{value: number; previous: number}>) =>
                    adjustPower(e.detail.value - e.detail.previous)}
                />
              </div>
              <Show when={heater()!.mode === 'automatic'}>
                <div class="control-group">
                  <div class="stat-label">Min Temp</div>
                  <void-number-input
                    controls="sides"
                    size="sm"
                    min={3} max={10} step={1} precision={0}
                    value={heater()!.startup_offset}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      setAutoOffsets(e.detail.value, heater()!.shutdown_offset)}
                  />
                </div>
                <div class="control-group">
                  <div class="stat-label">Max Temp</div>
                  <void-number-input
                    controls="sides"
                    size="sm"
                    min={3} max={10} step={1} precision={0}
                    value={heater()!.shutdown_offset}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      setAutoOffsets(heater()!.startup_offset, e.detail.value)}
                  />
                </div>
              </Show>
              <Show when={heater()!.mode !== 'fan'}>
                <div class="control-group">
                  <div class="stat-label">Altitude</div>
                  <void-button
                    variant={heater()!.altitude_mode ? 'filled' : 'outline'}
                    size="lg"
                    color={heater()!.altitude_mode ? 'caution' : 'default'}
                    onClick={toggleAltitude}>
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                      <path d="M3 20h18l-6.921 -14.612a2.3 2.3 0 0 0 -4.158 0l-6.921 14.612z" />
                      <path d="M7.5 11l2 2.5l2.5 -2.5l2 3l2.5 -2" />
                    </svg>
                  </void-button>
                </div>
              </Show>
            </div>
          </Show>
        </section>

      </main>
    </div>
  );
}
