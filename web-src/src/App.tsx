import { Show, For, createSignal, createEffect } from 'solid-js';
import '@voidable/ui';
import { createWs } from './ws';

export default function App() {
  const { connected, temperature, damper, heater, heaters, send } = createWs();
  const [scanning, setScanning] = createSignal(false);
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
          <div class="card-body">
            <Show when={damper()} fallback={<div class="stat-value">--</div>}>
              <div class="stat-value">{damper()!.angle.toFixed(1)}°</div>
              <Show when={damper()!.position !== null}>
                <div class="stat-label">Position {damper()!.position}</div>
              </Show>
            </Show>
          </div>
          <div class="card-actions">
            <void-button variant="outline" size="sm" onClick={() => setAngle(0)}>
              0°
            </void-button>
            <void-button variant="outline" size="sm" onClick={() => setAngle(45)}>
              45°
            </void-button>
            <void-button variant="outline" size="sm" onClick={() => setAngle(90)}>
              90°
            </void-button>
            <void-button variant="outline" size="sm" onClick={() => setAngle(135)}>
              135°
            </void-button>
            <void-button variant="outline" size="sm" onClick={() => setAngle(180)}>
              180°
            </void-button>
            <void-button variant="outline" size="sm" onClick={() => setAngle(270)}>
              270°
            </void-button>
            <void-button variant="filled" size="sm" color="info" onClick={setAuto}>
              Auto
            </void-button>
          </div>
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
                  <div class="stat-label">Exhaust</div>
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
              <div class="card-actions" style="margin-top: var(--void-space-4)">
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
