import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './index.css';

// Fenêtre embarquée (CEF) : le zoom navigateur n'a aucun sens ici et casse la
// mise en page — on neutralise Ctrl+molette et Ctrl +/-/0.
//
// Le garde `wheel` n'est POSÉ QUE pendant que Ctrl (ou Cmd) est enfoncé, et
// retiré dès qu'on relâche. Attaché en permanence à `window`, un écouteur
// `wheel` non passif interdit à Chromium de faire défiler sur le thread
// compositeur : chaque cran de molette doit d'abord passer par le thread
// principal pour savoir si le handler appellera `preventDefault()`. Le
// défilement devient tributaire du JS et se met à hoqueter.
//
// Vécu le 20/08/2026, signalé dans l'onglet Collections : c'est le premier
// écran assez long pour qu'on défile vraiment, donc le seul où ça se sentait.
// Le défaut, lui, était global.
const blockZoomWheel = (e: WheelEvent) => {
  if (e.ctrlKey || e.metaKey) e.preventDefault();
};

let wheelGuardArmed = false;

const armWheelGuard = () => {
  if (wheelGuardArmed) return;
  wheelGuardArmed = true;
  window.addEventListener('wheel', blockZoomWheel, { passive: false });
};

const disarmWheelGuard = () => {
  if (!wheelGuardArmed) return;
  wheelGuardArmed = false;
  window.removeEventListener('wheel', blockZoomWheel);
};

window.addEventListener('keydown', (e) => {
  // `keydown` part aussi quand Ctrl est pressé seul (key === 'Control'), donc
  // le garde est en place avant le premier cran de molette dans le cas normal.
  if (e.ctrlKey || e.metaKey) armWheelGuard();
  if ((e.ctrlKey || e.metaKey) && ['+', '-', '=', '0'].includes(e.key)) {
    e.preventDefault();
  }
});

window.addEventListener('keyup', (e) => {
  if (!e.ctrlKey && !e.metaKey) disarmWheelGuard();
});

// Ctrl relâché hors de la fenêtre : le `keyup` n'arrive jamais. Sans ça le
// garde resterait armé pour toute la session et le hoquet reviendrait.
window.addEventListener('blur', disarmWheelGuard);

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
