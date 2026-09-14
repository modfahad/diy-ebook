import { useState } from "react";
import Converter from "./Converter";
import Dashboard from "./Dashboard";
import Device from "./Device";
import Library from "./Library";
import Photos from "./Photos";
import { useSettings } from "./settings";
import "./App.css";

type Tab = "dashboard" | "library" | "device" | "photos" | "converter";

const TABS: Array<{ id: Tab; label: string; hint: string }> = [
  { id: "dashboard", label: "Dashboard", hint: "What works right now" },
  { id: "library", label: "Library", hint: "Packages on a card or folder" },
  { id: "device", label: "Device", hint: "Talk to the reader" },
  { id: "photos", label: "Photos", hint: "Home-screen photos and clock" },
  { id: "converter", label: "Converter", hint: "Documents into packages" },
];

export default function App() {
  const [tab, setTab] = useState<Tab>("dashboard");
  const [settings, update] = useSettings();

  return (
    <div className="app">
      <header className="app-head">
        <div>
          <h1>Quran Device</h1>
          <p className="muted">E-Ink Quran &amp; Islamic library -- desktop manager</p>
        </div>
        <nav className="tabs">
          {TABS.map((t) => (
            <button
              key={t.id}
              className={tab === t.id ? "tab tab-active" : "tab"}
              onClick={() => setTab(t.id)}
              title={t.hint}
              aria-current={tab === t.id ? "page" : undefined}
            >
              {t.label}
            </button>
          ))}
        </nav>
      </header>

      <main className="app-body">
        {tab === "dashboard" && <Dashboard settings={settings} onGoToTab={setTab} />}
        {tab === "library" && (
          <Library settings={settings} update={update} onGoToTab={setTab} />
        )}
        {tab === "device" && <Device settings={settings} update={update} />}
        {tab === "photos" && <Photos settings={settings} />}
        {tab === "converter" && <Converter settings={settings} update={update} />}
      </main>
    </div>
  );
}
