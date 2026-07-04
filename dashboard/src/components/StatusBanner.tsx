import type { DirectorState } from '../types';

interface Props {
    status: DirectorState['status'];
}

export function StatusBanner({ status }: Props) {
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
        </div>
    );
}
