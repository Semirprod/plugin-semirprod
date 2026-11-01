import { useEffect, useState } from 'react';

import { cn } from '@/lib/utils';

// Avatar du compte.
//
// L'image ne vient PAS de son hébergeur : le plugin la rapatrie et la sert sur
// `/avatar`. La CSP de la page interdit les images distantes, et on ne veut pas
// qu'un tiers apprenne l'IP du streamer ni l'heure de chaque ouverture du dock.
//
// Le repli d'un compte sans photo est le dégradé du site, servi par le même
// chemin. Cette page le dessinait elle-même, en niveaux de gris, sans rapport
// avec le dégradé coloré que le site tire de l'identifiant : le même compte
// apparaissait donc sous deux couleurs selon l'écran, ce qui défait ce à quoi
// sert cette couleur. Le dégradé est choisi à un seul endroit — voir
// `SemirprodDock::fetchDefaultAvatar()`.
//
// Ne reste ici qu'une pastille d'attente, volontairement SANS couleur : tant
// que le plugin n'a pas l'image, mieux vaut ne rien affirmer que réinventer une
// deuxième identité visuelle.
export function Avatar({
  src,
  name,
  email,
  className,
}: {
  src?: string;
  name?: string;
  email?: string;
  className?: string;
}) {
  const base = 'h-9 w-9 shrink-0 rounded-full';

  // Une image cassée (fichier illisible, 404 après changement de compte) doit
  // rendre la pastille, pas un trou. Masquer l'élément dans `onError` ne
  // suffisait pas : `src` restait renseigné, donc chaque rendu suivant
  // réaffichait la même image cassée, simplement invisible — la ligne perdait
  // son visuel jusqu'au redémarrage. L'échec est donc un ÉTAT.
  const [failed, setFailed] = useState(false);

  // Nouvelle source = nouvelle chance. L'URL porte une empreinte du contenu
  // (`/avatar?v=<hash>`) : un changement d'avatar change l'URL, et l'échec du
  // précédent ne doit pas condamner le suivant.
  useEffect(() => {
    setFailed(false);
  }, [src]);

  if (src && !failed) {
    return (
      <img
        src={src}
        alt=""
        className={cn(base, 'object-cover', className)}
        onError={() => setFailed(true)}
      />
    );
  }

  return (
    <div
      className={cn(
        base,
        // Un filet, pas un fond : le produit est noir, un aplat gris y ferait
        // une dalle. C'est la règle du reste de l'interface.
        'flex items-center justify-center border border-border',
        'text-[13px] font-semibold text-muted-foreground',
        className
      )}
    >
      {initials(name, email)}
    </div>
  );
}

// Deux lettres au plus : « Alex Martin » -> AM, « alex » -> A. Sur une adresse,
// la partie avant l'arobase, sinon tous les comptes d'un domaine partagent la
// même initiale. Mêmes règles de coupe que la route `/avatars/<id>` du site,
// pour que la pastille d'attente et l'image qui la remplace portent les mêmes
// lettres.
function initials(name?: string, email?: string): string {
  const from = (name || '').trim() || (email || '').split('@')[0].trim();
  if (!from) return '?';
  const mots = from.split(/[\s._-]+/).filter(Boolean);
  const lettres = mots.slice(0, 2).map((m) => m[0]);
  return lettres.join('').toUpperCase();
}
