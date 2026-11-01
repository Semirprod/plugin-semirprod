import * as React from 'react';

import { ChevronRight } from 'lucide-react';

import { cn } from '@/lib/utils';

// ---------------------------------------------------------------------------
// Primitives de mise en page. Toutes les surfaces sont NOIRES : la hiérarchie
// vient du filet (1 px, --border) et de la typo, jamais d'un fond gris. Les
// anciennes cartes empilaient un fond #141414 + des tuiles bordées à
// l'intérieur, ce qui donnait le fameux "gris" alors que le produit est 100 %
// noir. Ici : un titre hors du cadre, un seul cadre, des lignes séparées par
// des filets internes.
// ---------------------------------------------------------------------------

interface SectionProps {
  title: string;
  description?: string;
  actions?: React.ReactNode;
  children: React.ReactNode;
  className?: string;
}

export function Section({
  title,
  description,
  actions,
  children,
  className,
}: SectionProps) {
  return (
    <section className={cn('flex flex-col gap-2', className)}>
      <div className="flex items-start justify-between gap-3">
        <div>
          <h2 className="text-[11px] font-semibold uppercase tracking-[0.09em] text-muted-foreground">
            {title}
          </h2>
          {description && (
            <p className="mt-1 text-[12px] leading-snug text-muted-foreground">
              {description}
            </p>
          )}
        </div>
        {actions && <div className="shrink-0">{actions}</div>}
      </div>
      <div className="divide-y divide-border overflow-hidden rounded-lg border border-border">
        {children}
      </div>
    </section>
  );
}

// Ligne « libellé + aide » à gauche, contrôle à droite. Le contrôle reste
// aligné en haut pour que l'aide puisse tenir sur plusieurs lignes.
interface RowProps {
  title: string;
  hint?: React.ReactNode;
  control?: React.ReactNode;
  children?: React.ReactNode;
  tone?: 'default' | 'danger';
  /** Visuel avant le libellé (avatar, pastille). Optionnel : les lignes sans
   *  visuel gardent exactement leur mise en page. */
  leading?: React.ReactNode;
}

export function Row({
  title,
  hint,
  control,
  children,
  tone = 'default',
  leading,
}: RowProps) {
  return (
    <div className="px-4 py-3.5">
      <div className="flex items-start justify-between gap-4">
        {/* Le visuel colle au texte (gap-3), pas à l'écart du contrôle
            (gap-4) : sinon l'avatar flotte au milieu de la ligne. */}
        <div className="flex min-w-0 flex-1 items-center gap-3">
          {leading}
          <div className="min-w-0">
            <div
              className={cn(
                'text-sm font-medium',
                tone === 'danger' && 'text-destructive'
              )}
            >
              {title}
            </div>
            {hint && (
              <div className="mt-1 text-[12px] leading-snug text-muted-foreground">
                {hint}
              </div>
            )}
          </div>
        </div>
        {control && <div className="shrink-0 pt-0.5">{control}</div>}
      </div>
      {children && <div className="mt-3">{children}</div>}
    </div>
  );
}

// Ligne dédiée à un contrôle pleine largeur (select, champ texte) : le
// libellé passe au-dessus, le contrôle prend toute la largeur en dessous.
export function Field({
  label,
  hint,
  children,
}: {
  label: string;
  hint?: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <div className="px-4 py-3.5">
      <div className="text-sm font-medium">{label}</div>
      {hint && (
        <div className="mt-1 text-[12px] leading-snug text-muted-foreground">
          {hint}
        </div>
      )}
      <div className="mt-2">{children}</div>
    </div>
  );
}

// Message d'état à l'intérieur d'un cadre (liste vide, attente de données).
export function Empty({ children }: { children: React.ReactNode }) {
  return (
    <div className="px-4 py-3.5 text-[13px] leading-snug text-muted-foreground">
      {children}
    </div>
  );
}

// Grille de métriques. Les filets internes sont portés par les cellules ; le
// cadre du parent (overflow-hidden) rogne ceux du bord, donc pas de double
// trait visible ni de bordure orpheline quand la grille se replie en 2
// colonnes.
export function MetricGrid({
  children,
  className,
}: {
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <div className={cn('grid grid-cols-2 sm:grid-cols-4', className)}>
      {children}
    </div>
  );
}

export function Metric({
  label,
  value,
  meta,
  tone = 'default',
}: {
  label: string;
  value: string;
  meta?: string;
  tone?: 'default' | 'success' | 'warning' | 'danger';
}) {
  return (
    <div className="border-b border-r border-border px-4 py-3.5">
      <div className="text-[11px] uppercase tracking-[0.06em] text-muted-foreground">
        {label}
      </div>
      <div
        className={cn(
          'mt-1.5 text-[15px] font-semibold leading-none',
          tone === 'success' && 'text-[hsl(var(--success))]',
          tone === 'warning' && 'text-[hsl(var(--warning))]',
          tone === 'danger' && 'text-destructive'
        )}
      >
        {value}
      </div>
      {meta && (
        <div className="mt-1.5 text-[11px] leading-snug text-muted-foreground">
          {meta}
        </div>
      )}
    </div>
  );
}

// Pastille d'état. Même vocabulaire que le dock Qt : vert = OK, ambre =
// transitoire, rouge = cassé, gris = inconnu.
export function Dot({
  tone,
  pulse = false,
}: {
  tone: 'success' | 'warning' | 'danger' | 'neutral';
  pulse?: boolean;
}) {
  const cls =
    tone === 'success'
      ? 'bg-[hsl(var(--success))]'
      : tone === 'warning'
        ? 'bg-[hsl(var(--warning))]'
        : tone === 'danger'
          ? 'bg-destructive'
          : 'bg-muted-foreground';
  return (
    <span className="relative flex h-2 w-2 shrink-0">
      {pulse && (
        <span
          className={cn(
            'absolute inline-flex h-full w-full animate-ping rounded-full opacity-75',
            cls
          )}
        />
      )}
      <span className={cn('relative inline-flex h-2 w-2 rounded-full', cls)} />
    </span>
  );
}

// Ligne qui mene a une sous-page : toute la ligne est cliquable, la valeur
// actuelle a droite, un chevron pour dire « ca s'ouvre ». Un <button> et non
// une div cliquable : clavier et lecteur d'ecran suivent sans rien ajouter.
export function NavRow({
  title,
  hint,
  value,
  tone = 'default',
  onClick,
}: {
  title: string;
  hint?: React.ReactNode;
  value?: React.ReactNode;
  tone?: 'default' | 'danger';
  onClick: () => void;
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      className="flex w-full items-center justify-between gap-4 px-4 py-3.5 text-left transition-colors duration-150 hover:bg-accent/50 focus-visible:bg-accent/50 focus-visible:outline-none"
    >
      <div className="min-w-0">
        <div className={cn('truncate text-sm font-medium', tone === 'danger' && 'text-destructive')}>
          {title}
        </div>
        {hint && (
          <div className="mt-1 text-[12px] leading-snug text-muted-foreground">{hint}</div>
        )}
      </div>
      <div className="flex shrink-0 items-center gap-2 text-[12px] text-muted-foreground">
        {value}
        <ChevronRight className="h-4 w-4" aria-hidden />
      </div>
    </button>
  );
}
