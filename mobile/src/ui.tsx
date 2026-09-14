// Small building blocks shared by the tabs, mirroring desktop/src/ui.tsx.

import type { ReactNode } from 'react';
import { ActivityIndicator, Pressable, StyleSheet, Text, TextInput, View } from 'react-native';

export const colors = {
  ground: '#f6f5f1',
  card: '#ffffff',
  ink: '#1d1d1b',
  muted: '#6b6a64',
  line: '#e2e0d8',
  accent: '#1f6f5c',
  danger: '#a8322d',
};

export function Card({ title, children }: { title: string; children: ReactNode }) {
  return (
    <View style={styles.card}>
      <Text style={styles.cardTitle}>{title}</Text>
      {children}
    </View>
  );
}

export function Field({
  label,
  value,
  onChange,
  placeholder,
  secure,
  numeric,
}: {
  label: string;
  value: string;
  onChange: (text: string) => void;
  placeholder?: string;
  secure?: boolean;
  numeric?: boolean;
}) {
  return (
    <View style={styles.field}>
      <Text style={styles.label}>{label}</Text>
      <TextInput
        style={styles.input}
        value={value}
        onChangeText={onChange}
        placeholder={placeholder}
        placeholderTextColor={colors.muted}
        secureTextEntry={secure}
        autoCapitalize="none"
        autoCorrect={false}
        keyboardType={numeric ? 'number-pad' : 'default'}
      />
    </View>
  );
}

export function Button({
  title,
  onPress,
  busy,
  disabled,
  kind = 'primary',
}: {
  title: string;
  onPress: () => void;
  busy?: boolean;
  disabled?: boolean;
  kind?: 'primary' | 'secondary' | 'danger';
}) {
  const off = disabled || busy;
  return (
    <Pressable
      onPress={onPress}
      disabled={off}
      style={[styles.button, styles[kind], off ? styles.off : null]}
    >
      {busy ? (
        <ActivityIndicator color={kind === 'secondary' ? colors.ink : '#fff'} />
      ) : (
        <Text style={[styles.buttonText, kind === 'secondary' ? { color: colors.ink } : null]}>
          {title}
        </Text>
      )}
    </Pressable>
  );
}

export function KeyValues({ rows }: { rows: Array<[string, ReactNode]> }) {
  return (
    <View>
      {rows.map(([key, value]) => (
        <View key={key} style={styles.row}>
          <Text style={styles.key}>{key}</Text>
          <Text style={styles.value}>{value}</Text>
        </View>
      ))}
    </View>
  );
}

export function Meter({ value, total }: { value: number; total: number }) {
  const fraction = total > 0 ? Math.min(1, value / total) : 0;
  return (
    <View style={styles.meter}>
      <View style={[styles.meterFill, { width: `${fraction * 100}%` }]} />
    </View>
  );
}

export function Note({ children, tone = 'muted' }: { children: ReactNode; tone?: 'muted' | 'danger' }) {
  return <Text style={[styles.note, tone === 'danger' ? { color: colors.danger } : null]}>{children}</Text>;
}

export function describeError(error: unknown): string {
  if (error instanceof Error) return error.message;
  return String(error);
}

const styles = StyleSheet.create({
  card: {
    backgroundColor: colors.card,
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.line,
    padding: 16,
    marginBottom: 12,
    gap: 10,
  },
  cardTitle: { fontSize: 17, fontWeight: '600', color: colors.ink },
  field: { gap: 4 },
  label: { fontSize: 13, color: colors.muted },
  input: {
    borderWidth: 1,
    borderColor: colors.line,
    borderRadius: 8,
    paddingHorizontal: 12,
    paddingVertical: 10,
    fontSize: 16,
    color: colors.ink,
  },
  button: {
    borderRadius: 8,
    paddingVertical: 12,
    paddingHorizontal: 16,
    alignItems: 'center',
  },
  primary: { backgroundColor: colors.accent },
  secondary: { backgroundColor: colors.ground, borderWidth: 1, borderColor: colors.line },
  danger: { backgroundColor: colors.danger },
  off: { opacity: 0.5 },
  buttonText: { color: '#fff', fontSize: 16, fontWeight: '600' },
  row: { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 4, gap: 12 },
  key: { color: colors.muted, fontSize: 14 },
  value: { color: colors.ink, fontSize: 14, flexShrink: 1, textAlign: 'right' },
  meter: { height: 8, borderRadius: 4, backgroundColor: colors.line, overflow: 'hidden' },
  meterFill: { height: 8, backgroundColor: colors.accent },
  note: { color: colors.muted, fontSize: 13 },
});
