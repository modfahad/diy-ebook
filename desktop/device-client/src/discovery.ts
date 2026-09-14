// Device discovery.
//
// The spec asks for discovery over LAN/Wi-Fi. mDNS is the right mechanism and
// the device advertises `_quranreader._tcp` while it is in transfer mode --
// but nothing here can be verified without hardware on a real network, so the
// mechanism sits behind an interface and the only implementation that ships
// today is the manual one: the user types an address, which is also the
// fallback every mDNS setup needs anyway (guest networks and VLANs routinely
// block multicast).
//
// A real mDNS browser plugs in as another Discovery without touching callers.

import { DEFAULT_PORT, MDNS_PROTOCOL, MDNS_SERVICE } from '@quran-device/protocol';

export interface DiscoveredDevice {
  host: string;
  port: number;
  /** From the mDNS TXT record, or absent when the address was typed. */
  deviceId?: string;
  name?: string;
  source: 'mdns' | 'manual';
}

export interface Discovery {
  /** Collects devices for `timeoutMs`, then resolves. */
  find(timeoutMs: number): Promise<DiscoveredDevice[]>;
}

/** What the device advertises. Kept here so the firmware mirror has a source. */
export const SERVICE_NAME = `${MDNS_SERVICE}._${MDNS_PROTOCOL}`;

export class ManualDiscovery implements Discovery {
  constructor(private readonly addresses: string[]) {}

  async find(_timeoutMs = 0): Promise<DiscoveredDevice[]> {
    return this.addresses.map((address) => {
      const [host = address, port] = address.split(':');
      return {
        host,
        port: port ? Number(port) : DEFAULT_PORT,
        source: 'manual' as const,
      };
    });
  }
}

/**
 * Trusted devices the desktop remembers (spec section 19). Persistence is the
 * app's job; this is the shape and the merge rule.
 */
export interface TrustedDevice {
  deviceId: string;
  name: string;
  token: string;
  lastHost?: string;
  lastSeen?: number;
}

export class TrustStore {
  private readonly devices = new Map<string, TrustedDevice>();

  constructor(initial: TrustedDevice[] = []) {
    for (const device of initial) this.devices.set(device.deviceId, device);
  }

  get(deviceId: string): TrustedDevice | undefined {
    return this.devices.get(deviceId);
  }

  /**
   * Records a pairing. The token is only ever replaced by an explicit
   * re-pair: a device seen at a new address keeps the token it was paired
   * with, so moving between networks does not silently unpair it.
   */
  remember(device: TrustedDevice): void {
    const existing = this.devices.get(device.deviceId);
    this.devices.set(device.deviceId, {
      ...existing,
      ...device,
      token: device.token || existing?.token || '',
    });
  }

  forget(deviceId: string): boolean {
    return this.devices.delete(deviceId);
  }

  list(): TrustedDevice[] {
    return [...this.devices.values()];
  }
}
