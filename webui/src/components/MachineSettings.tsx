import { useEffect, useState } from 'react';

import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Field, Metric, MetricGrid, Row, Section } from '@/components/ui/section';
import { useT, type TFunction } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

function formatLatency(ms: number, t: TFunction): string {
  if (ms < 0) return t('machine.latencyUnmeasured');
  return `${ms} ms`;
}

function latencyTone(ms: number): 'default' | 'success' | 'warning' | 'danger' {
  if (ms < 0) return 'default';
  if (ms < 150) return 'success';
  if (ms < 400) return 'warning';
  return 'danger';
}

// Catégorie « Cette machine » : identité du poste et diagnostic réseau. Rien
// ici ne change ce qui est diffusé, d'où la séparation d'avec « Diffusion ».
export function MachineSettings({ state, send }: Props) {
  const t = useT();
  const [label, setLabel] = useState(state.instanceLabel || '');
  const [savedLabel, setSavedLabel] = useState(state.instanceLabel || '');

  // Le plugin est la source de vérité : si le libellé change ailleurs (autre
  // onglet, reset), on suit — sauf pendant une édition en cours.
  useEffect(() => {
    if (state.instanceLabel !== savedLabel) {
      setSavedLabel(state.instanceLabel || '');
      setLabel(state.instanceLabel || '');
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.instanceLabel]);

  const labelDirty =
    label.trim() !== (state.instanceLabel || '') && label.trim() !== '';

  const testDone =
    !state.connTestRunning &&
    (state.connTestApiMs >= 0 || state.connTestLiveMs >= 0);

  return (
    <Section
      title={t('machine.title')}
      description={t('machine.description')}
    >
      <Field
        label={t('machine.nameLabel')}
        hint={t('machine.nameHint')}
      >
        <div className="flex gap-2">
          <Input
            value={label}
            maxLength={64}
            onChange={(e) => setLabel(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter' && labelDirty) {
                send({ type: 'set_instance_label', label: label.trim() });
              }
            }}
            placeholder={t('machine.namePlaceholder')}
            className="h-9 bg-transparent"
          />
          <Button
            size="sm"
            variant="outline"
            disabled={!labelDirty}
            onClick={() => send({ type: 'set_instance_label', label: label.trim() })}
          >
            {t('machine.save')}
          </Button>
        </div>
      </Field>

      <Row
        title={t('machine.testTitle')}
        hint={t('machine.testHint')}
        control={
          <Button
            size="sm"
            variant="outline"
            disabled={state.connTestRunning}
            onClick={() => send({ type: 'run_conn_test' })}
          >
            {state.connTestRunning ? t('machine.testRunning') : t('machine.testRun')}
          </Button>
        }
      >
        {(testDone || state.connTestError) && (
          <div className="overflow-hidden rounded-md border border-border">
            <MetricGrid className="grid-cols-2 sm:grid-cols-2">
              <Metric
                label={t('machine.latencyBackend')}
                value={formatLatency(state.connTestApiMs, t)}
                tone={latencyTone(state.connTestApiMs)}
              />
              <Metric
                label={t('machine.latencyRealtime')}
                value={formatLatency(state.connTestLiveMs, t)}
                tone={latencyTone(state.connTestLiveMs)}
              />
            </MetricGrid>
          </div>
        )}
        {state.connTestError && (
          <div className="mt-2 text-[12px] text-destructive">
            {state.connTestError}
          </div>
        )}
      </Row>
    </Section>
  );
}
