import { Button } from '@/components/ui/button';
import { Switch } from '@/components/ui/switch';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Field, Row, Section } from '@/components/ui/section';
import { useT } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

// Catégorie « Scènes » : quelle scène OBS correspond à quel état du direct.
export function SceneCard({ state, send }: Props) {
  const t = useT();
  const scenes = state._scenes || [];

  const push = (patch: Partial<{ live: string; bug: string; autoSwitch: boolean }>) =>
    send({
      type: 'set_config',
      live: patch.live ?? state.sceneLive,
      bug: patch.bug ?? state.sceneBug,
      autoSwitch: patch.autoSwitch ?? state.autoSwitch,
    });

  const sceneSelect = (value: string, onChange: (v: string) => void) => (
    <Select value={value || undefined} onValueChange={onChange}>
      <SelectTrigger>
        <SelectValue placeholder={t('scenes.placeholder')} />
      </SelectTrigger>
      <SelectContent>
        {scenes.length === 0 ? (
          <SelectItem value={value || '__none'} disabled>
            {value || t('scenes.none')}
          </SelectItem>
        ) : (
          scenes.map((s) => (
            <SelectItem key={s} value={s}>
              {s}
            </SelectItem>
          ))
        )}
      </SelectContent>
    </Select>
  );

  // Bascule automatique en panne muette : la scène de direct est bien choisie
  // et bien présente, mais elle ne contient aucune source Semirprod. C'est la
  // condition que rien ne signalait, et que l'opérateur devait deviner.
  //
  // On n'y touche pas seul. Le plugin nomme le problème, dit où la source se
  // trouve si elle existe ailleurs, et propose le geste — c'est le clic qui
  // pose la source, jamais le chargement de la page.
  const liveSceneChosen = Boolean(state.sceneLive) && scenes.includes(state.sceneLive);
  const sourceMissing = liveSceneChosen && !state.sourceInLiveScene;
  const elsewhere = state.sourceScenes ?? [];

  return (
    <Section
      title={t('scenes.title')}
      description={t('scenes.description')}
    >
      <Field label={t('scenes.liveLabel')} hint={t('scenes.liveHint')}>
        {sceneSelect(state.sceneLive, (live) => push({ live }))}
      </Field>
      <Field
        label={t('scenes.fallbackLabel')}
        hint={t('scenes.fallbackHint')}
      >
        {sceneSelect(state.sceneBug, (bug) => push({ bug }))}
      </Field>
      <Row
        title={t('scenes.autoSwitchTitle')}
        hint={t('scenes.autoSwitchHint')}
        control={
          <Switch
            checked={state.autoSwitch}
            onCheckedChange={(autoSwitch) => push({ autoSwitch })}
          />
        }
      />
      {sourceMissing && (
        <Row
          tone="danger"
          title={t('scenes.sourceMissingTitle', { scene: state.sceneLive })}
          hint={
            elsewhere.length > 0
              ? t('scenes.sourceMissingElsewhere', { scenes: elsewhere.join(', ') })
              : t('scenes.sourceMissingHint')
          }
          control={
            <Button
              variant="outline"
              size="sm"
              onClick={() => send({ type: 'place_source_in_live_scene' })}
            >
              {t('scenes.fix')}
            </Button>
          }
        />
      )}
    </Section>
  );
}
