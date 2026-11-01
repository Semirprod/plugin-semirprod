import { useEffect, useState } from 'react';

import { Empty, Metric, MetricGrid, Section } from '@/components/ui/section';
import { intlTag, useLocale, useT, type Locale } from '@/i18n';
import type { PluginState } from '@/lib/ws';

function formatUptime(startedAt: string): string {
  if (!startedAt) return '';
  const start = Date.parse(startedAt);
  if (Number.isNaN(start)) return '';
  const s = Math.max(0, Math.floor((Date.now() - start) / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h ${String(m).padStart(2, '0')}m`;
  return `${m}m ${String(sec).padStart(2, '0')}s`;
}

// Séparateur de milliers : « 182 400 » se lit, « 182400 » non.
function formatCount(n: number, locale: Locale): string {
  return n.toLocaleString(intlTag(locale));
}

function formatBitrate(kbps: number): string {
  if (kbps <= 0) return '0 kb/s';
  if (kbps >= 1000) return `${(kbps / 1000).toFixed(1)} Mb/s`;
  return `${kbps} kb/s`;
}

export function StatsCard({ state }: { state: PluginState }) {
  const t = useT();
  const locale = useLocale();
  // Re-render l'uptime chaque seconde pendant une diffusion.
  const [, tick] = useState(0);
  useEffect(() => {
    if (!state.streaming) return;
    const id = setInterval(() => tick((n) => n + 1), 1000);
    return () => clearInterval(id);
  }, [state.streaming]);

  if (!state.statsActive && !state.streaming && !state.recording) {
    return (
      <Section title={t('stats.title')}>
        <Empty>{t('stats.idle')}</Empty>
      </Section>
    );
  }

  const droppedPct =
    state.statsTotalFrames > 0
      ? (state.statsDroppedFrames / state.statsTotalFrames) * 100
      : 0;
  const congestionPct = Math.round((state.statsCongestion || 0) * 100);

  return (
    <Section title={t('stats.title')}>
      <MetricGrid>
        <Metric
          label={t('stats.state')}
          value={state.streaming ? t('stats.running') : t('stats.stopped')}
          meta={state.streaming ? formatUptime(state.streamingStartedAt) : undefined}
          tone={state.streaming ? 'success' : 'default'}
        />
        <Metric
          label={t('stats.bitrate')}
          value={formatBitrate(state.statsKbps)}
          meta={t('stats.bitrateMeta')}
        />
        <Metric
          label={t('stats.dropped')}
          value={t('fmt.percent', {
            value: state.statsTotalFrames > 0 ? droppedPct.toFixed(1) : '0',
          })}
          meta={
            state.statsTotalFrames > 0
              ? `${formatCount(state.statsDroppedFrames, locale)} / ${formatCount(state.statsTotalFrames, locale)}`
              : t('stats.droppedWaiting')
          }
          tone={droppedPct >= 5 ? 'danger' : droppedPct >= 1 ? 'warning' : 'default'}
        />
        <Metric
          label={t('stats.congestion')}
          value={t('fmt.percent', { value: state.statsActive ? congestionPct : 0 })}
          meta={t('stats.congestionMeta')}
          tone={
            congestionPct >= 50 ? 'danger' : congestionPct >= 20 ? 'warning' : 'default'
          }
        />
      </MetricGrid>
    </Section>
  );
}
