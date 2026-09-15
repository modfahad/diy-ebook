// The Android app: the desktop app's tabs on a phone (docs/android.md).

import { StatusBar } from 'expo-status-bar';
import { useState } from 'react';
import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';

import Converter from './src/Converter';
import Dashboard from './src/Dashboard';
import Device from './src/Device';
import Library from './src/Library';
import Photos from './src/Photos';
import { RenderWorkerProvider } from './src/render/RenderWorker';
import { useSettings } from './src/settings';
import { colors } from './src/ui';

const TABS = ['Dashboard', 'Library', 'Device', 'Converter', 'Photos'] as const;
type Tab = (typeof TABS)[number];

export default function App() {
  const [tab, setTab] = useState<Tab>('Dashboard');
  const { settings, loaded, update } = useSettings();

  return (
    <SafeAreaProvider>
      <RenderWorkerProvider>
        <SafeAreaView style={styles.screen} edges={['top', 'bottom']}>
          <StatusBar style="dark" />
          <View style={styles.body}>
            {!loaded ? (
              <ActivityIndicator style={{ marginTop: 40 }} />
            ) : tab === 'Dashboard' ? (
              <Dashboard settings={settings} update={update} />
            ) : tab === 'Library' ? (
              <Library settings={settings} />
            ) : tab === 'Device' ? (
              <Device settings={settings} />
            ) : tab === 'Converter' ? (
              <Converter settings={settings} />
            ) : (
              <Photos settings={settings} />
            )}
          </View>
          <View style={styles.tabs}>
            {TABS.map((name) => (
              <Pressable key={name} style={styles.tab} onPress={() => setTab(name)}>
                <Text style={[styles.tabText, name === tab ? styles.tabActive : null]}>{name}</Text>
              </Pressable>
            ))}
          </View>
        </SafeAreaView>
      </RenderWorkerProvider>
    </SafeAreaProvider>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: colors.ground },
  body: { flex: 1 },
  tabs: {
    flexDirection: 'row',
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: colors.line,
    backgroundColor: colors.card,
  },
  tab: { flex: 1, paddingVertical: 14, alignItems: 'center' },
  tabText: { fontSize: 12, color: colors.muted },
  tabActive: { color: colors.accent, fontWeight: '700' },
});
