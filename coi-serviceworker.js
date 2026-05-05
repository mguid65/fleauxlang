/*
 * Cross-origin isolation helper adapted from coi-serviceworker.
 * Serves COOP/COEP headers from a service worker for static hosts like GitHub Pages.
 */
if (typeof window === 'undefined') {
  self.addEventListener('install', () => {
    self.skipWaiting();
  });

  self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
  });

  self.addEventListener('message', (event) => {
    if (event.data && event.data.type === 'deregister') {
      self.registration.unregister().then(() => {
        return self.clients.matchAll();
      }).then((clients) => {
        for (const client of clients) {
          client.navigate(client.url);
        }
      });
    }
  });

  self.addEventListener('fetch', (event) => {
    const request = event.request;

    if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') {
      return;
    }

    event.respondWith((async () => {
      let response;
      try {
        response = await fetch(request);
      } catch (error) {
        if (request.mode === 'navigate') {
          return new Response(null, {
            status: 302,
            headers: {
              Location: request.url,
            },
          });
        }
        throw error;
      }

      if (response.status === 0) {
        return response;
      }

      const headers = new Headers(response.headers);
      headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
      headers.set('Cross-Origin-Opener-Policy', 'same-origin');
      headers.set('Cross-Origin-Resource-Policy', 'cross-origin');

      return new Response(response.body, {
        status: response.status,
        statusText: response.statusText,
        headers,
      });
    })());
  });
} else {
  (() => {
    const coi = {
      shouldRegister() {
        return !window.crossOriginIsolated && window.isSecureContext;
      },
      shouldDeregister() {
        return false;
      },
      doReload() {
        window.location.reload();
      },
      quiet: false,
      ...window.coi,
    };

    const log = coi.quiet ? () => undefined : console.log.bind(console);
    const n = window.navigator;

    if (coi.shouldDeregister()) {
      if (!n.serviceWorker?.controller) {
        return;
      }
      n.serviceWorker.controller.postMessage({ type: 'deregister' });
      return;
    }

    if (!coi.shouldRegister()) {
      return;
    }

    if (!n.serviceWorker) {
      log('COI service worker not registered because service workers are unavailable in this browser.');
      return;
    }

    n.serviceWorker.register(window.document.currentScript.src, {
      scope: window.coiScope || window.document.currentScript.dataset.scope || '.',
    }).then((registration) => {
      log('COI service worker registered.', registration.scope);

      registration.addEventListener('updatefound', () => {
        log('Reloading page to use updated cross-origin isolation service worker.');
      });

      if (registration.active && !n.serviceWorker.controller) {
        coi.doReload();
      }
    }).catch((error) => {
      console.error('COI service worker registration failed.', error);
    });

    window.addEventListener('load', () => {
      if (n.serviceWorker.controller && !window.crossOriginIsolated) {
        log('Reloading page to activate cross-origin isolation.');
        coi.doReload();
      }
    });
  })();
}

