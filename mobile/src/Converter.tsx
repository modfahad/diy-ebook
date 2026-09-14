// Converter: PDF, EPUB and TXT into packages on the phone.
// Milestone 7 in docs/android.md -- not built yet.

import { ScrollView } from 'react-native';

import { Card, Note } from './ui';

export default function Converter() {
  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      <Card title="Converter">
        <Note>
          Converting books on the phone is coming: TXT and EPUB first, then PDF (docs/android.md,
          milestone 7). Until then, convert on the desktop app.
        </Note>
      </Card>
    </ScrollView>
  );
}
