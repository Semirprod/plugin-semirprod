import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Switch } from '@/components/ui/switch';
import { Empty, Field, Row, Section } from '@/components/ui/section';
import { useT } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

// Catégorie « Diffusion » : tout ce qui décide de ce qui part vers les
// serveurs. Le choix du flux vit ici, pas dans une carte séparée : c'est le
// premier réglage de diffusion, pas un sujet à part.
export function DiffusionSettings({ state, send }: Props) {
  const t = useT();
  const streams = state.streams || [];
  const active = streams.find((s) => s.active);

  return (
    <Section
      title={t('broadcast.title')}
      description={t('broadcast.description')}
    >
      {state.connected &&
        (streams.length === 0 ? (
          <Empty>{t('broadcast.noStreams')}</Empty>
        ) : (
          <Field
            label={t('broadcast.streamLabel')}
            hint={t('broadcast.streamHint')}
          >
            {/* Menu déroulant Radix (et non <select> natif) : le <select> de
                CEF s'ouvre avec le rendu gris du système, hors thème. */}
            <Select
              value={active?.id ?? undefined}
              onValueChange={(id) => send({ type: 'select_stream', id })}
            >
              <SelectTrigger>
                <SelectValue placeholder={t('broadcast.streamPlaceholder')} />
              </SelectTrigger>
              <SelectContent>
                {streams.map((s) => (
                  <SelectItem key={s.id} value={s.id}>
                    {s.name || t('common.unnamedStream')}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </Field>
        ))}

      <Row
        title={t('broadcast.previewTitle')}
        hint={t('broadcast.previewHint')}
        control={
          <Switch
            checked={state.previewEnabled ?? true}
            onCheckedChange={(on) => send({ type: 'set_preview_enabled', on })}
          />
        }
      />

    </Section>
  );
}
