import { Empty, Section } from '@/components/ui/section';
import { intlTag, useLocale, useT, type Locale, type TFunction } from '@/i18n';
import { cn } from '@/lib/utils';
import type { PluginState, SessionRecord } from '@/lib/ws';

function formatDuration(sec: number): string {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  if (h > 0) return `${h} h ${String(m).padStart(2, '0')}`;
  return `${m} min`;
}

function formatDate(iso: string, locale: Locale, t: TFunction): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return t('common.unknown');
  return d.toLocaleString(intlTag(locale), {
    day: '2-digit',
    month: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

function formatBitrate(kbps: number): string {
  if (kbps <= 0) return '0 kb/s';
  if (kbps >= 1000) return `${(kbps / 1000).toFixed(1)} Mb/s`;
  return `${kbps} kb/s`;
}

function Row({ s }: { s: SessionRecord }) {
  const t = useT();
  const locale = useLocale();
  const clean = s.incidents === 0 && s.droppedPct < 1;
  return (
    <div className="px-4 py-3.5">
      <div className="flex items-baseline justify-between gap-3">
        <div className="text-sm font-medium">{formatDate(s.endedAt, locale, t)}</div>
        <div className="text-[12px] text-muted-foreground">
          {formatDuration(s.durationSec)}
        </div>
      </div>
      <div className="mt-1.5 flex flex-wrap gap-x-4 gap-y-1 text-[12px] text-muted-foreground">
        <span>{t('sessions.avgBitrate', { value: formatBitrate(s.avgKbps) })}</span>
        <span>{t('sessions.peak', { value: formatBitrate(s.peakKbps) })}</span>
        <span className={cn(s.droppedPct >= 1 && 'text-[hsl(var(--warning))]')}>
          {t('sessions.droppedPct', { pct: s.droppedPct.toFixed(1) })}
        </span>
        <span
          className={cn(
            s.incidents > 0
              ? 'text-[hsl(var(--warning))]'
              : 'text-[hsl(var(--success))]'
          )}
        >
          {clean ? t('sessions.clean') : t('sessions.incidents', { count: s.incidents })}
        </span>
      </div>
    </div>
  );
}

export function SessionsCard({ state }: { state: PluginState }) {
  const t = useT();
  const sessions = state.sessions || [];
  return (
    <Section title={t('sessions.title')}>
      {sessions.length === 0 ? (
        <Empty>{t('sessions.empty')}</Empty>
      ) : (
        sessions.map((s) => <Row key={s.endedAt} s={s} />)
      )}
    </Section>
  );
}
