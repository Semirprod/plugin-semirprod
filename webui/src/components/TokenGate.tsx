import { useState } from 'react';
import { KeyRound } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { useT } from '@/i18n';

interface Props {
  onSubmit: (token: string) => void;
}

// Shown until the WS bearer handshake succeeds. The token rotates every OBS
// restart and is surfaced by the dock's "Copier le token" button + the OBS log.
export function TokenGate({ onSubmit }: Props) {
  const t = useT();
  const [raw, setRaw] = useState('');

  const submit = () => {
    const trimmed = raw.trim();
    if (!trimmed) return;
    const token = trimmed.startsWith('bearer-') ? trimmed.slice(7) : trimmed;
    // Le jeton n'est PLUS écrit dans l'URL. `window.location.hash = token`
    // l'affichait en clair dans la barre d'adresse du navigateur, l'inscrivait
    // dans l'historique et dans la restauration de session, et le rendait
    // lisible par tout script de la page via `document.URL`. Le modèle
    // documenté côté C++ (WebUiServer.hpp) dit explicitement que le jeton ne
    // doit jamais être rendu à l'écran, précisément parce que les streamers
    // partagent leur fenêtre OBS et leur second écran.
    onSubmit(token);
  };

  return (
    <div className="mx-auto w-full max-w-sm rounded-lg border border-border p-8">
      <div className="flex flex-col items-center gap-4 text-center">
        <div className="flex h-10 w-10 items-center justify-center rounded-full border border-border">
          <KeyRound className="h-4 w-4 text-muted-foreground" />
        </div>
        <div>
          <h2 className="text-base font-semibold">{t('token.title')}</h2>
          <p className="mt-1 text-[13px] leading-snug text-muted-foreground">
            {t('token.description')}
          </p>
        </div>
        <Input
          value={raw}
          onChange={(e) => setRaw(e.target.value)}
          onKeyDown={(e) => e.key === 'Enter' && submit()}
          placeholder="bearer-…"
          autoComplete="off"
          spellCheck={false}
          className="bg-transparent text-center font-mono"
        />
        <Button className="w-full" onClick={submit}>
          {t('token.submit')}
        </Button>
        <p className="text-[11px] text-muted-foreground">
          {t('token.note')}
        </p>
      </div>
    </div>
  );
}
