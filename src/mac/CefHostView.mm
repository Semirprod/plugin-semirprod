#include "mac/CefHostView.hpp"

#import <AppKit/AppKit.h>

#include <obs-module.h>

namespace semirprod {

namespace {

// La WId d'un QWidget natif sur macOS EST le pointeur NSView. Le cast est donc
// direct — mais il n'est valide que si le widget a bien une fenetre native,
// d'ou le controle d'existence chez l'appelant.
NSView* toView(WId hostView) {
  return (__bridge NSView*)reinterpret_cast<void*>(hostView);
}

}  // namespace

void logCefHostView(WId hostView, const char* context) {
  NSView* host = toView(hostView);
  if (host == nil) {
    blog(LOG_WARNING, "[semirprod-cefview] %s : pas de NSView hote", context);
    return;
  }

  const NSRect hostFrame = host.frame;
  NSArray<NSView*>* subs = host.subviews;
  // L'ADRESSE de la NSView, et pas seulement sa taille : c'est elle qui dit si
  // Qt a recree la vue entre deux affichages (adresse differente) ou si c'est
  // CEF/AppKit qui a retire sa sous-vue d'une vue restee la meme. Les deux
  // causes appellent des corrections opposees, et rien d'autre ne les
  // distingue.
  blog(LOG_INFO,
       "[semirprod-cefview] %s : hote=%p %.0fx%.0f, %lu sous-vue(s), fenetre=%s",
       context, (__bridge void*)host, hostFrame.size.width, hostFrame.size.height,
       (unsigned long)subs.count, host.window ? "oui" : "NON");

  NSUInteger i = 0;
  for (NSView* sub in subs) {
    const NSRect f = sub.frame;
    blog(LOG_INFO,
         "[semirprod-cefview]   [%lu] %s cadre=%.0f,%.0f %.0fx%.0f masquee=%d",
         (unsigned long)i++, sub.className.UTF8String, f.origin.x, f.origin.y,
         f.size.width, f.size.height, sub.isHidden ? 1 : 0);
  }
}

bool refitCefHostView(WId hostView, QSize size) {
  NSView* host = toView(hostView);
  if (host == nil) return false;

  NSArray<NSView*>* subs = host.subviews;
  if (subs.count == 0) return false;

  const NSRect target = NSMakeRect(0, 0, size.width(), size.height());
  for (NSView* sub in subs) {
    // L'autoresizing mask est pose a chaque passage, volontairement : il n'est
    // pas garanti d'avoir survecu a un detachement/rattachement, et le poser
    // deux fois ne coute rien.
    sub.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    sub.frame = target;
    sub.hidden = NO;
    [sub setNeedsDisplay:YES];
  }
  [host setNeedsDisplay:YES];
  return true;
}

}  // namespace semirprod
