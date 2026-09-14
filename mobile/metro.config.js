// Metro, pointed at the shared TypeScript libraries (docs/android.md).
//
// package.json links them with file: dependencies, which npm installs as
// symlinks into node_modules/@quran-device. Metro resolves a symlink to its
// real path, so each library's real folder has to be watched too -- one copy
// of each library, not a fork. They are consumed as built (dist/): run
// `npm run build` in each first, as the desktop does.

const path = require('path');
const { getDefaultConfig } = require('expo/metro-config');

const repoRoot = path.resolve(__dirname, '..');
const config = getDefaultConfig(__dirname);

config.watchFolders = [
  path.join(repoRoot, 'packages/protocol'),
  path.join(repoRoot, 'packages/qpk-format'),
  path.join(repoRoot, 'desktop/device-client'),
  path.join(repoRoot, 'desktop/converter'),
];

// A shared library's own dependencies (@noble/hashes, fflate) resolve from its
// node_modules; anything it does not have falls back to the app's.
config.resolver.nodeModulesPaths = [path.join(__dirname, 'node_modules')];

module.exports = config;
