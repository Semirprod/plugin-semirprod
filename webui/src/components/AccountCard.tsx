import { Avatar } from '@/components/ui/avatar';
import { Button } from '@/components/ui/button';
import { Row, Section } from '@/components/ui/section';
import { useT } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

export function AccountCard({ state, send }: Props) {
  const t = useT();
  // Cette carte n'est rendue que compte relié : l'écran d'appairage
  // (PairingGate) tient tout l'avant, elle n'a plus d'état « déconnecté » à
  // gérer.
  //
  // Elle affichait `accountLabel`, qui porte le CODE D'APPAIRAGE. Ce champ se
  // vide une fois le compte lié : l'écran retombait sur « Aucun compte relié »
  // alors qu'un compte l'était bel et bien, et rien ne disait LEQUEL.
  //
  // Or le plugin publie sur les streams de l'organisation de ce compte. Sur un
  // poste partagé, ou après un ré-appairage, c'est la première chose à pouvoir
  // vérifier, avant qu'un direct ne parte au mauvais endroit.
  const titre = state.accountName || state.accountEmail || t('account.linked');

  // Le nom en titre, l'adresse en dessous : c'est le nom qu'on reconnaît, mais
  // c'est l'adresse qui départage deux comptes homonymes. Si l'un des deux
  // manque, on ne répète pas l'autre.
  const detail =
    state.accountName && state.accountEmail
      ? state.accountEmail
      : t('account.sessionActive');

  return (
    <Section title={t('account.title')}>
      <Row
        leading={
          <Avatar
            src={state.accountAvatar}
            name={state.accountName}
            email={state.accountEmail}
          />
        }
        title={titre}
        hint={
          // `truncate` : une adresse est un mot insécable, sans coupe elle
          // passait sous les boutons dans un panneau étroit.
          <span className="block truncate">{detail}</span>
        }
        control={
          // Pas de bouton « Rafraîchir » : l'accès Studio est poussé par le
          // serveur quand l'abonnement change, la liste des scènes suit les
          // événements OBS. Un bouton n'aurait rien à déclencher, et invitait
          // à marteler l'API.
          //
          // Et RIEN DU TOUT dans une instance infogérée : le compte y vient de
          // l'appairage de l'instance, pas du client. Se déconnecter le
          // couperait de son propre OBS — plus de flux, plus de pilotage
          // depuis le site — sans aucun moyen de se rappairer depuis l'écran
          // distant. Le plugin refuse la commande de son côté ; ici on ne
          // montre pas un bouton qui ne peut mener qu'à une panne.
          state.kiosk ? null : (
            <Button
              size="sm"
              variant="outline"
              onClick={() => send({ type: 'disconnect' })}
            >
              {t('account.disconnect')}
            </Button>
          )
        }
      />
    </Section>
  );
}
