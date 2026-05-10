import { Show } from 'solid-js';
import '@voidable/ui';
import { createWs } from './ws';

export default function App() {
  const { connected, temperature, damper, heater, send } = createWs();

  const setAngle = (angle: number) => send({ type: 'damper.set', angle });
  const setAuto = () => send({ type: 'damper.set', auto: true });
  const scanHeaters = () => send({ type: 'heaters.scan', timeout: 5 });
  const disconnectHeater = () => send({ type: 'heaters.disconnect' });

  return (
    <div class="shell">
      {/* Nav */}
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
        {/* Temperature Card */}
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

        {/* Damper Card */}
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

        {/* Heater Card */}
        <section class="card">
          <div class="card-header">
            <span class="card-title">Heater</span>
            <Show when={heater()}>
              <void-badge color={heater()!.connected ? 'success' : 'default'}>
                {heater()!.connected ? 'Connected' : 'Disconnected'}
              </void-badge>
            </Show>
          </div>
          <div class="card-body">
            <Show when={heater()?.connected} fallback={
              <div class="card-actions">
                <void-button variant="filled" size="sm" onClick={scanHeaters}>
                  Scan for Heaters
                </void-button>
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
