// The app side of the render worker (render-worker/worker.ts): a hidden
// WebView mounted once at the root, and a promise-based call() for the tabs.

import { createContext, useCallback, useContext, useMemo, useRef, useState, type ReactNode } from 'react';
import { View } from 'react-native';
import { WebView, type WebViewMessageEvent } from 'react-native-webview';

import { decodeBase64, encodeBase64 } from '../base64';
import workerHtml from './workerHtml.generated';

const PIECE_CHARS = 512 * 1024;

export interface WorkerProgress {
  stage: string;
  done?: number;
  total?: number;
}

interface Pending {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
  onProgress: ((progress: WorkerProgress) => void) | undefined;
  pieces: string[];
}

export interface RenderWorker {
  ready: boolean;
  /**
   * Runs a worker command. `bytes` are sent first under generated keys, and
   * each arg named in `bytes` is replaced by its key. A result carrying
   * `bytesPieces` gets its `bytes` field back as a Uint8Array.
   */
  call<T>(
    command: string,
    args?: Record<string, unknown>,
    options?: { bytes?: Record<string, Uint8Array>; onProgress?: (progress: WorkerProgress) => void },
  ): Promise<T>;
}

const Context = createContext<RenderWorker | null>(null);

export function useRenderWorker(): RenderWorker {
  const worker = useContext(Context);
  if (!worker) throw new Error('useRenderWorker must be used inside <RenderWorkerProvider>');
  return worker;
}

/** How long a call waits for the page to (re)start before giving up. */
const START_TIMEOUT_MS = 60_000;

export function RenderWorkerProvider({ children }: { children: ReactNode }) {
  const view = useRef<WebView>(null);
  const pending = useRef(new Map<number, Pending>());
  const nextId = useRef(1);
  const [ready, setReady] = useState(false);
  const readyNow = useRef(false);
  const waiters = useRef(new Set<() => void>());
  // Bumped to mount a fresh WebView: once Android has killed a WebView's
  // renderer -- which it does to background apps, and opening the file
  // picker backgrounds this one -- that WebView cannot be reloaded, only
  // replaced.
  const [generation, setGeneration] = useState(0);

  /** Fails every call the page was working on: its answers will never come. */
  const failPending = useCallback((reason: string) => {
    const lost = [...pending.current.values()];
    pending.current.clear();
    for (const entry of lost) entry.reject(new Error(reason));
  }, []);

  const restart = useCallback(() => {
    readyNow.current = false;
    setReady(false);
    failPending('the converter was closed by Android to save memory; try again');
    setGeneration((current) => current + 1);
  }, [failPending]);

  const whenReady = useCallback(
    () =>
      readyNow.current
        ? Promise.resolve()
        : new Promise<void>((resolve, reject) => {
            const done = () => {
              clearTimeout(timer);
              waiters.current.delete(done);
              resolve();
            };
            const timer = setTimeout(() => {
              waiters.current.delete(done);
              reject(new Error('the converter did not start; close and reopen the app'));
            }, START_TIMEOUT_MS);
            waiters.current.add(done);
          }),
    [],
  );

  const onMessage = useCallback((event: WebViewMessageEvent) => {
    let message: { id: number; kind: string; [key: string]: unknown };
    try {
      message = JSON.parse(event.nativeEvent.data);
    } catch {
      return;
    }
    if (message.kind === 'ready') {
      // A fresh page: whatever an earlier page was doing is gone with it.
      failPending('the converter restarted; try again');
      readyNow.current = true;
      setReady(true);
      for (const wake of [...waiters.current]) wake();
      return;
    }
    const entry = pending.current.get(message.id);
    if (!entry) return;
    if (message.kind === 'progress') {
      entry.onProgress?.(message.progress as WorkerProgress);
    } else if (message.kind === 'piece') {
      entry.pieces.push(String(message.piece));
    } else if (message.kind === 'error') {
      pending.current.delete(message.id);
      entry.reject(new Error(String(message.message)));
    } else if (message.kind === 'done') {
      pending.current.delete(message.id);
      const result = message.result as Record<string, unknown> | null;
      if (result && result.bytesPieces) {
        entry.resolve({ ...result, bytes: decodeBase64(entry.pieces.join('')) });
      } else {
        entry.resolve(result);
      }
    }
  }, [failPending]);

  const call = useCallback<RenderWorker['call']>(async (command, args = {}, options = {}) => {
    // Sending into a page that is still starting would lose the message.
    await whenReady();
    const webview = view.current;
    if (!webview) throw new Error('the render worker is not running');
    const id = nextId.current++;
    const finalArgs: Record<string, unknown> = { ...args };
    for (const [name, bytes] of Object.entries(options.bytes ?? {})) {
      const key = `${id}-${name}`;
      const text = encodeBase64(bytes);
      for (let at = 0; at < text.length; at += PIECE_CHARS) {
        webview.injectJavaScript(
          `window.__renderWorker.put(${JSON.stringify(key)}, ${JSON.stringify(text.slice(at, at + PIECE_CHARS))}); true;`,
        );
      }
      if (text.length === 0) {
        webview.injectJavaScript(`window.__renderWorker.put(${JSON.stringify(key)}, ""); true;`);
      }
      finalArgs[name] = key;
    }
    return new Promise((resolve, reject) => {
      pending.current.set(id, {
        resolve: resolve as (value: unknown) => void,
        reject,
        onProgress: options.onProgress,
        pieces: [],
      });
      webview.injectJavaScript(
        `window.__renderWorker.run(${JSON.stringify({ id, command, args: finalArgs })}); true;`,
      );
    });
  }, [whenReady]);

  const value = useMemo<RenderWorker>(() => ({ ready, call }), [ready, call]);

  return (
    <Context.Provider value={value}>
      {children}
      {/* Kept mounted but invisible: a zero-size WebView may be suspended. */}
      <View pointerEvents="none" style={{ position: 'absolute', width: 2, height: 2, opacity: 0, left: -10, top: -10 }}>
        <WebView
          key={generation}
          ref={view}
          source={{ html: workerHtml, baseUrl: 'https://render-worker.local/' }}
          originWhitelist={['*']}
          javaScriptEnabled
          onMessage={onMessage}
          onContentProcessDidTerminate={restart}
          onRenderProcessGone={restart}
        />
      </View>
    </Context.Provider>
  );
}
