// Library: open packages on the phone, validate them, show covers.
// Milestone 6 in docs/android.md -- not built yet.

import { ScrollView } from 'react-native';

import { Card, Note } from './ui';

export default function Library() {
  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      <Card title="Library">
        <Note>Opening and checking packages on the phone is coming (docs/android.md, milestone 6).</Note>
        <Note>To install a package on the device now, use the Device tab.</Note>
      </Card>
    </ScrollView>
  );
}
