import type { DirectorState } from '../types';

interface Props {
    status: DirectorState['status'];
}

declare global {
    interface Window {
        ne_setTickEnabled?: (arg: string) => void;
    }
}

export function StatusBanner({ status }: Props) {
    const onToggleTick = (e: React.ChangeEvent<HTMLInputElement>) => {
        const next = e.target.checked;
        // C++ side registers this JS listener via
        // PrismaUI_API::RegisterJSListener("ne_setTickEnabled"). We push a
        // stringified bool; the C++ side flips Tick's atomic. Next full-
        // state push will confirm the new value via `tick_enabled`.
        window.ne_setTickEnabled?.(next ? 'true' : 'false');
    };

    return (
        <div className="status-banner">
            <span className={status.skyrim_net_available ? 'pill ok' : 'pill bad'}>
                SkyrimNet {status.skyrim_net_available ? `v${status.skyrim_net_version}` : 'unavailable'}
            </span>
            <span className={status.prisma_ui_available ? 'pill ok' : 'pill bad'}>
                PrismaUI {status.prisma_ui_available ? 'ok' : 'unavailable'}
            </span>
            <span className={status.director_enabled ? 'pill ok' : 'pill bad'}>
                Director {status.director_enabled ? 'on' : 'off'}
            </span>
            <label className="tick-toggle" title="When off, the plugin skips its periodic poll: no phase advance, no evaluation, no dispatcher tick. Debug aid.">
                <input
                    type="checkbox"
                    checked={status.tick_enabled}
                    onChange={onToggleTick}
                />
                <span>Tick</span>
            </label>
        </div>
    );
}
