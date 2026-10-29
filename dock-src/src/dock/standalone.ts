// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// Entry of the standalone Stage dock (`pnpm build:standalone` -> Parallaxa data/dock/dock-standalone.html,
// one self-contained file opened as a file URL in an OBS Custom Browser Dock).
// The global stylesheet without components.css (the dock uses none of its components);
// standalone.html declares the layer order.
import '../styles/reset.css';
import '../styles/tokens.css';
import '../styles/base.css';
import '../styles/utilities.css';
import { mount } from 'svelte';
import { ObsHost } from './obs-host.svelte.ts';
import Standalone from './Standalone.svelte';
import { translator } from './standalone-i18n.ts';
import { setTranslator } from './t.ts';

const host = new ObsHost();
setTranslator(translator(() => host.lang));
// standalone.html's <html> already carries the `layout-dock` class.
mount(Standalone, { target: document.getElementById('app') as HTMLElement, props: { host } });
// obs-websocket may run without a password: the Hello says whether one is needed.
host.connect();
