import { Show, For, createSignal, createEffect, createMemo } from 'solid-js';
import '@voidable/ui';
import { createWs, type HeaterState } from './ws';

export default function App() {
  const {
    connected, damper, heaters, devices,
    pendingByName, ota, send, sendCmd,
  } = createWs();

  const [sliding, setSliding] = createSignal(false);
  const [localAngle, setLocalAngle] = createSignal(0);
  const [damperTab, setDamperTab] = createSignal<'control' | 'config'>('control');
  const [selectedName, setSelectedName] = createSignal('');
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
  const syncVoidSelect = (
    el: HTMLElement,
    options: Array<{value: string; label: string}>,
    selected: string,
  ) => {
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

  /** Union of every heater name we've ever seen — connected, scanned,
   *  or historical. Used for the damper's "Heater ID" config select so
   *  the user can bind the damper to a heater that isn't currently in
   *  range. */
  const [knownNames, setKnownNames] = createSignal<string[]>([]);
  createEffect(() => {
    const fromDevices = devices().map(d => d.name);
    const fromHeaters = Object.keys(heaters());
    setKnownNames(prev => {
      const set = new Set([...prev, ...fromDevices, ...fromHeaters]);
      return Array.from(set);
    });
  });

  /** Connected heaters sorted by name — the dropdown's option list.
   *  The equality check returns the same array reference when the
   *  set of names is unchanged across telemetry updates, so the
   *  combobox doesn't re-render and close its popup. */
  const connectedNames = createMemo(
    () =>
      Object.values(heaters())
        .map(h => h.name)
        .sort((a, b) => a.localeCompare(b)),
    [],
    {
      equals: (a, b) =>
        a.length === b.length && a.every((v, i) => v === b[i]),
    },
  );

  /** Auto-select first connected heater whenever the current selection
   *  isn't connected (or no selection yet). */
  createEffect(() => {
    const names = connectedNames();
    const sel = selectedName();
    if (!sel || !names.includes(sel)) {
      setSelectedName(names[0] ?? '');
    }
  });

  /** The heater that the controls in the card body apply to. */
  const selectedHeater = createMemo<HeaterState | undefined>(() =>
    heaters()[selectedName()],
  );

  createEffect(() => {
    const names = knownNames();
    const selected = damper()?.heater_name ?? '';
    if (!configHeaterSelectRef) return;
    syncVoidSelect(
      configHeaterSelectRef,
      [{value: '', label: 'None'}, ...names.map(n => ({value: n, label: n}))],
      selected,
    );
  });

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
  const heaterCmd = (cmd: Record<string, unknown>) => {
    const t = selectedName();
    if (!t) return;
    send({ type: 'heater.command', target: t, ...cmd });
  };
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

  /** Pending command tied to the currently-selected heater. */
  const pendingForSelected = () => pendingByName()[selectedName()];
  const isPending = (field: string, value?: boolean | string) => {
    const p = pendingForSelected();
    if (!p || p.field !== field) return false;
    if (value === undefined) return true;
    return p.value === value;
  };

  const loadConfig = () => {
    send({ type: 'damper.status' });
    send({ type: 'heaters.list' });
    send({ type: 'ota.status' });
  };
  createEffect(() => { if (connected()) loadConfig(); });

  const otaCheck = () => sendCmd({ type: 'ota.check' });
  const otaBusy = () => {
    const s = ota()?.state;
    return s === 'checking' || s === 'downloading' || s === 'verifying';
  };
  const otaProgress = () => {
    const o = ota();
    if (!o || o.bytes_total === 0) return 0;
    return Math.round((o.bytes_received / o.bytes_total) * 100);
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
                  <Show when={knownNames().length} fallback={
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
            <Show when={connectedNames().length}>
              <void-combobox
                size="lg"
                placeholder="Select heater"
                value={selectedName()}
                on:void-change={(e: CustomEvent<{value: string}>) =>
                  setSelectedName(e.detail.value)}>
                <For each={connectedNames()}>
                  {n => <void-option value={n}>{n}</void-option>}
                </For>
              </void-combobox>
            </Show>
          </div>
          <div class="card-body">
            <Show when={selectedHeater()} fallback={
              <div class="stat-value heater-placeholder">
                Scanning for heaters...
              </div>
            }>
              <Show when={selectedHeater()!.error}>
                <void-alert color="error" variant="subtle">
                  E{selectedHeater()!.error} — {heaterError(selectedHeater()!.error)}
                </void-alert>
              </Show>
              <Show when={!selectedHeater()!.connected}>
                <void-alert color="default" variant="subtle">
                  Waiting for reconnect to {selectedHeater()!.name}...
                </void-alert>
              </Show>
              <void-collapsible heading="Status">
                <div class="stat-grid">
                  <void-stat size="sm" label="System" value={selectedHeater()!.power} />
                  <void-stat size="sm" label="State" value={selectedHeater()!.step} />
                  <void-stat size="sm" label="Core" value={`${selectedHeater()!.core_temp.toFixed(1)}°C`} />
                  <void-stat size="sm" label="Ambient" value={`${selectedHeater()!.ambient_temp.toFixed(1)}°C`} />
                  <void-stat size="sm" label="Voltage" value={`${selectedHeater()!.voltage.toFixed(1)}V`} />
                  <void-stat size="sm" label="RSSI" value={`${selectedHeater()!.ble_rssi_dbm} dBm`} />
                </div>
              </void-collapsible>
            </Show>
          </div>
          <Show when={selectedHeater()?.connected}>
            <div class="heater-controls">
              <div class="control-group">
                <div class="stat-label">Power</div>
                <div class="control-row">
                  <void-button variant={selectedHeater()!.power === 'OFF' ? 'outline' : 'filled'}
                    size="lg" color="success"
                    onClick={() => setPower(true)}>
                    <Show when={isPending('power', true)} fallback="On">
                      <void-spinner size="sm" /> On
                    </Show>
                  </void-button>
                  <void-button variant={selectedHeater()!.power === 'OFF' ? 'filled' : 'outline'}
                    size="lg" color="error"
                    onClick={() => setPower(false)}>
                    <Show when={isPending('power', false)} fallback="Off">
                      <void-spinner size="sm" /> Off
                    </Show>
                  </void-button>
                </div>
              </div>
              <div class="control-group">
                <div class="stat-label">Mode</div>
                <void-toggle-group
                  value={selectedHeater()!.mode}
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
                  {selectedHeater()!.mode === 'automatic' ? 'Target Temp' : 'Power Level'}
                </div>
                <void-number-input
                  controls="sides"
                  size="lg"
                  min={selectedHeater()!.mode === 'automatic' ? 8 : 1}
                  max={selectedHeater()!.mode === 'automatic' ? 36 : 10}
                  step={1} precision={0}
                  value={selectedHeater()!.power_level}
                  on:void-change={(e: CustomEvent<{value: number; previous: number}>) =>
                    adjustPower(e.detail.value - e.detail.previous)}
                />
              </div>
              <Show when={selectedHeater()!.mode === 'automatic'}>
                <div class="control-group">
                  <div class="stat-label">Min Temp</div>
                  <void-number-input
                    controls="sides"
                    size="sm"
                    min={3} max={10} step={1} precision={0}
                    value={selectedHeater()!.startup_offset}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      setAutoOffsets(e.detail.value, selectedHeater()!.shutdown_offset)}
                  />
                </div>
                <div class="control-group">
                  <div class="stat-label">Max Temp</div>
                  <void-number-input
                    controls="sides"
                    size="sm"
                    min={3} max={10} step={1} precision={0}
                    value={selectedHeater()!.shutdown_offset}
                    on:void-change={(e: CustomEvent<{value: number}>) =>
                      setAutoOffsets(selectedHeater()!.startup_offset, e.detail.value)}
                  />
                </div>
              </Show>
              <Show when={selectedHeater()!.mode !== 'fan'}>
                <div class="control-group">
                  <div class="stat-label">Altitude</div>
                  <void-button
                    variant={selectedHeater()!.altitude_mode ? 'filled' : 'outline'}
                    size="lg"
                    color={selectedHeater()!.altitude_mode ? 'caution' : 'default'}
                    onClick={toggleAltitude}>
                    <Show when={isPending('altitude')} fallback={
                      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                        <path d="M3 20h18l-6.921 -14.612a2.3 2.3 0 0 0 -4.158 0l-6.921 14.612z" />
                        <path d="M7.5 11l2 2.5l2.5 -2.5l2 3l2.5 -2" />
                      </svg>
                    }>
                      <void-spinner size="sm" />
                    </Show>
                  </void-button>
                </div>
              </Show>
            </div>
          </Show>
        </section>

        <section class="card" data-testid="ota-card">
          <div class="card-header">
            <span class="card-title">Firmware</span>
            <Show when={ota()}>
              <void-badge
                color={
                  ota()!.state === 'failed' ? 'error' :
                  ota()!.state === 'up_to_date' ? 'success' :
                  ota()!.state === 'swap_pending' ? 'caution' :
                  otaBusy() ? 'default' : 'default'
                }>
                {ota()!.state.replace('_', ' ')}
              </void-badge>
            </Show>
          </div>
          <div class="card-body">
            <div class="stat-grid">
              <void-stat size="sm" label="Running"
                value={ota()?.running_version || '—'}
                data-testid="ota-running" />
              <void-stat size="sm" label="Latest"
                value={ota()?.available_version || '—'}
                data-testid="ota-available" />
            </div>
            <Show when={otaBusy() && ota()!.bytes_total > 0}>
              <div class="ota-progress">
                <div class="stat-label">
                  {ota()!.bytes_received.toLocaleString()} / {ota()!.bytes_total.toLocaleString()} bytes ({otaProgress()}%)
                </div>
                <progress max={ota()!.bytes_total}
                  value={ota()!.bytes_received}
                  data-testid="ota-progress" />
              </div>
            </Show>
            <Show when={ota()?.state === 'failed' && ota()!.error}>
              <void-alert color="error" variant="subtle"
                data-testid="ota-error">
                {ota()!.error}
              </void-alert>
            </Show>
            <Show when={ota()?.state === 'swap_pending'}>
              <void-alert color="caution" variant="subtle">
                Update installed. Device rebooting...
              </void-alert>
            </Show>
          </div>
          <div class="card-actions">
            <void-button
              size="lg" variant="filled" color="default"
              disabled={otaBusy() || undefined}
              data-testid="ota-check"
              onClick={otaCheck}>
              <Show when={otaBusy()} fallback="Check for updates">
                <void-spinner size="sm" /> Checking
              </Show>
            </void-button>
          </div>
        </section>
      </main>
    </div>
  );
}
