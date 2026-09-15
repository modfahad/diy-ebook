// Converter: a PDF, EPUB, TXT or Quran JSON file in, a validated package out
// -- the desktop Converter tab on a phone (docs/android.md milestone 7).
//
// The whole conversion runs in the render worker, the same converter code the
// desktop's bridge runs, with pdf.js and a canvas for PDF page pictures. Nothing
// is saved or sent unless the package validates.

import * as DocumentPicker from 'expo-document-picker';
import * as Sharing from 'expo-sharing';
import { File } from 'expo-file-system';
import { useState } from 'react';
import { Image, Pressable, ScrollView, Switch, Text, View } from 'react-native';

import { COVER_HEIGHT, COVER_WIDTH } from '@quran-device/qpk-format';

import { decodeBase64, encodeBase64 } from './base64';
import { clientFor } from './deviceClient';
import { formatBytes } from './format';
import { saveToLibrary } from './packageStore';
import { pageImagePng } from './pageImage';
import { levelsPngDataUrl } from './png';
import { useRenderWorker, type WorkerProgress } from './render/RenderWorker';
import type { Settings } from './settings';
import { Button, Card, colors, describeError, Field, KeyValues, Meter, Note } from './ui';

interface Preview {
  pageNumber: number;
  pageCount: number;
  heading?: string;
  text: string;
}

interface ConvertReply {
  bytes: Uint8Array;
  report: {
    contentType: string;
    packageSize: number;
    pageCount: number;
    chapterCount: number;
    wordCount: number;
    warnings: string[];
  };
  validation: { ok: boolean; errors: string[]; warnings: string[]; summary?: { contentId: string } };
  preview: Preview | { error: string } | null;
}

function isPreview(value: ConvertReply['preview']): value is Preview {
  return value !== null && !('error' in value);
}

function describeProgress(progress: WorkerProgress): string {
  if (progress.stage === 'rendering' && progress.total) {
    return `Rendering page ${progress.done} of ${progress.total}`;
  }
  return progress.stage === 'converting' ? 'Converting and validating' : progress.stage;
}

function Toggle({ label, value, onChange, disabled }: { label: string; value: boolean; onChange: (v: boolean) => void; disabled?: boolean }) {
  return (
    <Pressable
      onPress={() => !disabled && onChange(!value)}
      style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', opacity: disabled ? 0.5 : 1 }}
    >
      <Text style={{ color: colors.ink, flex: 1 }}>{label}</Text>
      <Switch value={value} onValueChange={onChange} disabled={disabled} />
    </Pressable>
  );
}

export default function Converter({ settings }: { settings: Settings }) {
  const worker = useRenderWorker();
  const [source, setSource] = useState<{ name: string; bytes: Uint8Array } | null>(null);
  const [title, setTitle] = useState('');
  const [author, setAuthor] = useState('');
  const [language, setLanguage] = useState('');
  const [contentVersion, setContentVersion] = useState('1');
  const [keepLayout, setKeepLayout] = useState(true);
  const [trimMargins, setTrimMargins] = useState(true);
  const [singleChapter, setSingleChapter] = useState(false);
  const [cover, setCover] = useState<{ levels: Uint8Array; origin: 'epub' | 'picture' } | null>(null);
  const [coverNote, setCoverNote] = useState<string | null>(null);

  const [result, setResult] = useState<ConvertReply | null>(null);
  const [preview, setPreview] = useState<Preview | null>(null);
  const [pagePicture, setPagePicture] = useState<string | null>(null);
  const [stage, setStage] = useState<string | null>(null);
  const [upload, setUpload] = useState<{ sent: number; total: number } | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const isPdf = source !== null && /\.pdf$/iu.test(source.name);
  const isEpub = source !== null && /\.epub$/iu.test(source.name);
  const ready = settings.deviceHost.trim() !== '' && settings.deviceToken.trim() !== '';

  const run = async (what: string, action: () => Promise<string | void>) => {
    setBusy(what);
    setError(null);
    setMessage(null);
    try {
      const done = await action();
      if (done) setMessage(done);
    } catch (caught) {
      setError(describeError(caught));
    } finally {
      setBusy(null);
      setStage(null);
    }
  };

  const showPage = (bytes: Uint8Array, value: Preview) => {
    setPreview(value);
    try {
      setPagePicture(pageImagePng(bytes, value.pageNumber));
    } catch {
      setPagePicture(null);
    }
  };

  const pickSource = () =>
    run('pick', async () => {
      const picked = await DocumentPicker.getDocumentAsync({ copyToCacheDirectory: true, multiple: false });
      if (picked.canceled || picked.assets.length === 0) return;
      const asset = picked.assets[0]!;
      const bytes = new Uint8Array(await new File(asset.uri).arrayBuffer());
      setSource({ name: asset.name, bytes });
      setResult(null);
      setPreview(null);
      setPagePicture(null);
      setCover((current) => (current?.origin === 'picture' ? current : null));
      setCoverNote(null);
      if (/\.epub$/iu.test(asset.name)) {
        const found = await worker.call<{ levels: string } | null>('epubCover', {}, { bytes: { key: bytes } });
        if (found) {
          setCover((current) => (current?.origin === 'picture' ? current : { levels: decodeBase64(found.levels), origin: 'epub' }));
          setCoverNote('Cover taken from the EPUB.');
        } else {
          setCoverNote('This EPUB declares no cover picture. Choose one, or the device draws a cover.');
        }
      }
      return `${asset.name}, ${formatBytes(bytes.length)}`;
    });

  const chooseCover = () =>
    run('cover', async () => {
      const picked = await DocumentPicker.getDocumentAsync({ type: 'image/*', copyToCacheDirectory: true });
      if (picked.canceled || picked.assets.length === 0) return;
      const asset = picked.assets[0]!;
      const bytes = new Uint8Array(await new File(asset.uri).arrayBuffer());
      const reply = await worker.call<{ levels: string }>(
        'coverFromPicture',
        { mediaType: asset.mimeType ?? '' },
        { bytes: { key: bytes } },
      );
      setCover({ levels: decodeBase64(reply.levels), origin: 'picture' });
      setCoverNote(`Cover from ${asset.name}.`);
    });

  const convert = () =>
    run('convert', async () => {
      if (!source) return;
      setResult(null);
      setPreview(null);
      setPagePicture(null);
      setStage('Starting');
      const version = Number.parseInt(contentVersion, 10);
      const options: Record<string, unknown> = { filename: source.name };
      if (title.trim()) options.title = title.trim();
      if (author.trim()) options.author = author.trim();
      if (language.trim()) options.language = language.trim();
      if (Number.isFinite(version) && version > 1) options.contentVersion = version;
      if (singleChapter) options.singleChapter = true;
      if (cover) options.cover = encodeBase64(cover.levels);
      const reply = await worker.call<ConvertReply>(
        'convert',
        { options, keepLayout: isPdf && keepLayout, trimMargins, previewPage: 1 },
        { bytes: { key: source.bytes }, onProgress: (progress) => setStage(describeProgress(progress)) },
      );
      setResult(reply);
      if (isPreview(reply.preview)) showPage(reply.bytes, reply.preview);
      return reply.validation.ok
        ? `Package ready: ${formatBytes(reply.bytes.length)}. Save it, send it, or share it.`
        : 'This package does not validate, so it cannot be saved or sent.';
    });

  const goToPage = (page: number) =>
    run('page', async () => {
      if (!result) return;
      showPage(result.bytes, await worker.call<Preview>('preview', { page }));
    });

  const outputName = () => `${(title.trim() || source?.name.replace(/\.[^.]+$/u, '') || 'book').slice(0, 60)}.qpk`;

  const save = () =>
    run('save', async () => {
      if (!result?.validation.ok) return;
      const file = saveToLibrary(result.bytes, outputName());
      return `Saved to the Library tab as ${file.name}.`;
    });

  const send = () =>
    run('send', async () => {
      if (!result?.validation.ok) return;
      setUpload({ sent: 0, total: result.bytes.length });
      try {
        const reply = await clientFor(settings).uploadPackage(result.bytes, {
          onProgress: (p) => setUpload({ sent: p.sentBytes, total: p.totalBytes }),
        });
        return `Installed on the device (${reply.location}).`;
      } finally {
        setUpload(null);
      }
    });

  const share = () =>
    run('share', async () => {
      if (!result?.validation.ok) return;
      const file = saveToLibrary(result.bytes, outputName());
      await Sharing.shareAsync(file.uri, { mimeType: 'application/octet-stream' });
    });

  const report = result?.report;
  const validation = result?.validation;
  const pageCount = report?.pageCount ?? 0;
  const page = preview?.pageNumber ?? 1;

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }} keyboardShouldPersistTaps="handled">
      <Card title="Source">
        <Note>PDF, EPUB, TXT, or a structured Quran JSON source.</Note>
        <Button
          title={source ? `Change file (${source.name})` : 'Choose file…'}
          onPress={pickSource}
          busy={busy === 'pick'}
          disabled={busy !== null || !worker.ready}
        />
        {!worker.ready ? <Note>Starting the converter…</Note> : null}
        <Field label="Title (overrides the document's)" value={title} onChange={setTitle} />
        <Field label="Author" value={author} onChange={setAuthor} />
        <Field label="Language (e.g. en, ar)" value={language} onChange={setLanguage} />
        <Field label="Content version" value={contentVersion} onChange={setContentVersion} numeric />

        {isPdf ? (
          <>
            <Toggle label="Keep the PDF's page layout (pages as pictures)" value={keepLayout} onChange={setKeepLayout} />
            <Toggle label="Trim margins (bigger text on the device)" value={trimMargins} onChange={setTrimMargins} disabled={!keepLayout} />
          </>
        ) : null}
        <Toggle label="One chapter (skip structure detection)" value={singleChapter} onChange={setSingleChapter} />

        <View style={{ flexDirection: 'row', gap: 12, alignItems: 'flex-start' }}>
          {cover ? (
            <Image
              source={{ uri: levelsPngDataUrl(COVER_WIDTH, COVER_HEIGHT, cover.levels) }}
              style={{ width: 81, height: 108, borderWidth: 1, borderColor: colors.line }}
            />
          ) : (
            <View style={{ width: 81, height: 108, borderWidth: 2, borderColor: colors.ink, alignItems: 'center', justifyContent: 'center' }}>
              <Text style={{ fontSize: 10, color: colors.ink }}>No cover</Text>
            </View>
          )}
          <View style={{ flex: 1, gap: 8 }}>
            <Button title="Choose cover…" kind="secondary" onPress={chooseCover} busy={busy === 'cover'} disabled={busy !== null || !worker.ready} />
            <Button title="Remove cover" kind="secondary" onPress={() => setCover(null)} disabled={!cover || busy !== null} />
            {coverNote ? <Note>{coverNote}</Note> : null}
            {!isEpub && !cover ? <Note>For a PDF or TXT, choose a picture; without one the device draws a book icon.</Note> : null}
          </View>
        </View>

        <Button title={busy === 'convert' && stage ? `${stage}…` : 'Convert'} onPress={convert} busy={busy === 'convert' && !stage} disabled={!source || busy !== null || !worker.ready} />
        {busy === 'convert' && stage ? <Note>{stage}…</Note> : null}
        {error ? <Note tone="danger">{error}</Note> : null}
        {message ? <Note>{message}</Note> : null}
      </Card>

      {result && validation && report ? (
        <Card title={validation.ok ? 'Package ready' : 'Does not validate'}>
          {!validation.ok ? <Note tone="danger">{validation.errors.map((line) => `• ${line}`).join('\n')}</Note> : null}
          <KeyValues
            rows={[
              ['Type', report.contentType],
              ['Size', formatBytes(result.bytes.length)],
              ['Pages', String(report.pageCount)],
              ['Chapters', String(report.chapterCount)],
              ['Words', report.wordCount.toLocaleString()],
              ['Content id', validation.summary?.contentId ?? '-'],
            ]}
          />
          {[...report.warnings, ...validation.warnings].length > 0 ? (
            <Note>{[...report.warnings, ...validation.warnings].map((line) => `• ${line}`).join('\n')}</Note>
          ) : null}
          {upload ? <Meter value={upload.sent} total={upload.total} /> : null}
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
            <Button title="Save to library" onPress={save} busy={busy === 'save'} disabled={!validation.ok || busy !== null} />
            <Button title="Send to device" onPress={send} busy={busy === 'send'} disabled={!validation.ok || !ready || busy !== null} />
            <Button title="Share" kind="secondary" onPress={share} busy={busy === 'share'} disabled={!validation.ok || busy !== null} />
          </View>
        </Card>
      ) : null}

      {result && preview ? (
        <Card title={`Preview, page ${page} of ${pageCount}`}>
          <View style={{ flexDirection: 'row', gap: 8 }}>
            <Button title="Previous" kind="secondary" onPress={() => goToPage(page - 1)} disabled={page <= 1 || busy !== null} />
            <Button title="Next" kind="secondary" onPress={() => goToPage(page + 1)} disabled={page >= pageCount || busy !== null} />
          </View>
          {pagePicture ? (
            <>
              <Image source={{ uri: pagePicture }} style={{ width: 240, height: 400, borderWidth: 1, borderColor: colors.line }} />
              <Note>This page exactly as the device shows it, held upright.</Note>
            </>
          ) : (
            <>
              {preview.heading ? <Text style={{ fontWeight: '600', color: colors.ink }}>{preview.heading}</Text> : null}
              <Text style={{ fontFamily: 'monospace', fontSize: 12, color: colors.ink }}>{preview.text}</Text>
            </>
          )}
        </Card>
      ) : null}
    </ScrollView>
  );
}
