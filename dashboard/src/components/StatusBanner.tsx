import { language, pluralsAvailable, t } from '../i18n/strings';
import type { DirectorState } from '../types';

interface Props {
    status: DirectorState['status'];
}

export function StatusBanner({ status }: Props) {
    const lang = language();
    return (
        <div className="status-banner">
            <span className={status.skyrim_net_available ? 'pill ok' : 'pill bad'}>
                {status.skyrim_net_available
                    ? t('status.skyrimnet_version', { version: status.skyrim_net_version })
                    : t('status.skyrimnet_unavailable')}
            </span>
            <span className={status.prisma_ui_available ? 'pill ok' : 'pill bad'}>
                {status.prisma_ui_available ? t('status.prismaui_ok') : t('status.prismaui_unavailable')}
            </span>
            <span className={status.director_enabled ? 'pill ok' : 'pill bad'}>
                {status.director_enabled ? t('status.director_on') : t('status.director_off')}
            </span>
            {/* The game draws its own text with its own fonts; this view
                does not, so it can say what the game cannot show. */}
            {lang?.fonts.verdict === 'not_covered' && (
                <span className="pill bad">
                    {t('status.fonts_not_covered', { font_set: lang.fonts.font_set, language: lang.native_name })}
                </span>
            )}
            {!pluralsAvailable() && <span className="pill bad">{t('status.plurals_unavailable')}</span>}
        </div>
    );
}
