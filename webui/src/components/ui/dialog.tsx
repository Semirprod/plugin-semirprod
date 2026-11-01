import * as React from 'react';
import * as DialogPrimitive from '@radix-ui/react-dialog';
import { X } from 'lucide-react';

import { useT } from '@/i18n';
import { cn } from '@/lib/utils';

// ---------------------------------------------------------------------------
// Fenêtre par-dessus la page : fond assombri, cadre noir à filet 1 px (même
// règle que Section : la hiérarchie vient du filet, pas d'un fond gris ni
// d'une ombre). Radix porte le focus, Échap, le clic à côté et le blocage du
// défilement. Pas d'animation d'entrée.
//
// Le corps défile seul : l'en-tête (titre + fermer) et le pied (boutons)
// restent visibles quelle que soit la longueur de la liste.
// ---------------------------------------------------------------------------

export function Modal({
  open,
  onClose,
  title,
  description,
  footer,
  children,
}: {
  open: boolean;
  onClose: () => void;
  title: string;
  description?: React.ReactNode;
  /** Boutons du bas, alignés à droite. */
  footer?: React.ReactNode;
  children: React.ReactNode;
}) {
  const t = useT();
  return (
    <DialogPrimitive.Root open={open} onOpenChange={(ouvert) => !ouvert && onClose()}>
      <DialogPrimitive.Portal>
        <DialogPrimitive.Overlay className="fixed inset-0 z-50 bg-black/70" />
        <DialogPrimitive.Content
          className={cn(
            'fixed left-1/2 top-1/2 z-50 flex max-h-[85vh] w-[calc(100%-32px)] max-w-md -translate-x-1/2 -translate-y-1/2 flex-col',
            'rounded-lg border border-border bg-background focus:outline-none',
          )}
        >
          <div className="flex items-start justify-between gap-4 border-b border-border px-4 py-3">
            <div className="min-w-0">
              <DialogPrimitive.Title className="truncate text-sm font-semibold">
                {title}
              </DialogPrimitive.Title>
              {description ? (
                <DialogPrimitive.Description className="mt-1 text-[12px] leading-snug text-muted-foreground">
                  {description}
                </DialogPrimitive.Description>
              ) : (
                // Radix avertit en console sans description : une copie du
                // titre, lue seulement par les lecteurs d'écran.
                <DialogPrimitive.Description className="sr-only">{title}</DialogPrimitive.Description>
              )}
            </div>
            <DialogPrimitive.Close
              className="-mr-1 shrink-0 rounded-md p-1 text-muted-foreground transition-colors duration-150 hover:text-foreground focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-ring"
              aria-label={t('common.close')}
            >
              <X className="h-4 w-4" />
            </DialogPrimitive.Close>
          </div>
          <div className="min-h-0 flex-1 divide-y divide-border overflow-y-auto">{children}</div>
          {footer && (
            <div className="flex justify-end gap-2 border-t border-border px-4 py-3">{footer}</div>
          )}
        </DialogPrimitive.Content>
      </DialogPrimitive.Portal>
    </DialogPrimitive.Root>
  );
}
