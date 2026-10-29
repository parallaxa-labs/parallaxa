# Parallaxa user guide

Parallaxa is a free OBS Studio plugin (GPL) for 2D VTubers: depth and parallax across your scene layers, virtual camera moves (zoom, pan, rotate, shake, automatic drift, audio-reactive punch), colour looks, effects and per-layer depth blur. It works on its own with any avatar source (VTube Studio through Spout2, a PNGTuber image, a webcam). Apps such as AI Live Studio can drive it remotely over obs-websocket (`docs/protocol/vendor-api.md`).

日本語: [../ja/guide.md](../ja/guide.md) · Bahasa Indonesia: [../id/guide.md](../id/guide.md)

## 1. What you need

- Windows 10 (1803 or later) or 11, 64-bit
- OBS Studio 32.0 or later (Steam or the installer from obsproject.com)

## 2. Install

1. **Close OBS.**
2. Run `Parallaxa-<version>-Setup.exe` and accept the administrator prompt.
3. On the **OBS Studio** page, check the folder. Setup finds the official installer, Steam libraries and `C:\Program Files\obs-studio`. If you use another copy, click **Browse** and pick the folder that contains `bin\64bit\obs64.exe`.
4. Setup reads the OBS version and picks the plugin layout:
   - **OBS 33 or later:** `C:\ProgramData\obs-studio\plugins\parallaxa\parallaxa.dll`
   - **OBS 32:** `C:\ProgramData\obs-studio\plugins\parallaxa\bin\64bit\parallaxa.dll`
   - **OBS not found:** both copies (OBS 33+ uses the first one and ignores the second).

   On OBS 32, **Also install the copy for OBS 33 and later** is ticked by default, so both copies are installed. Steam updates OBS by itself, and this keeps the plugin loading after the update to 33. OBS 32 ignores the extra copy.
5. Start OBS. To check the install, open **Help > Log Files > View Current Log** and look for `[parallaxa] loaded`.

**Manual install (zip).** Extract `Parallaxa-<version>-windows-x64.zip` into `C:\ProgramData\obs-studio\plugins\`, so that you get `...\plugins\parallaxa\`. The zip holds both layouts, so it works on OBS 32 and 33.

## 3. First scene in 10 seconds

In OBS choose **Tools > Parallaxa: Set up current scene**. Parallaxa adds the **Parallaxa Camera Rig** and **Parallaxa FX** filters to the current scene. It gives each item a role from its name: `bg`/`background` is the background, `vts`/`spout`/`avatar`/`model` is the character, `fg`/`foreground` is the foreground. Then it applies the Cozy Chat look. Fine-tune each layer's depth in the filter properties. For depth of field, add **Parallaxa Depth Blur** to the background or foreground source; the camera rig sets its strength from the layer's distance to the focus.

To try a ready-made scene, choose **Tools > Parallaxa: Import sample scene**. It adds the scene **Parallaxa Sample - Just Chatting** (original background and foreground art, a VTube Studio Spout2 source, the Parallaxa filters) to your current scene collection. The images load from the plugin's data folder, so OBS asks for no missing files. If a scene or source of the same name already exists, the new one gets a number, such as "Background 2"; nothing you have is overwritten. The same collection is also in `data\presets\samples\parallaxa_sample.json` for **Scene Collection > Import**.

## 4. The Parallaxa dock

The dock controls the camera, looks, effects and layers without opening filter properties. Add it in **Docks > Custom Browser Docks** with this URL:

```
file:///C:/ProgramData/obs-studio/plugins/parallaxa/data/dock/dock-standalone.html
```

It talks to OBS directly over obs-websocket. Turn on the WebSocket server first (**Tools > WebSocket Server Settings**, port 4455); the dock asks for its password once.

## 5. VTube Studio through Spout2

Spout2 shares the avatar with OBS as a GPU texture with a transparent background, without the VTube Studio UI.

1. Install the **OBS Spout2 plugin** by Off World Live (https://github.com/Off-World-Live/obs-spout2-plugin/releases) for your OBS version:
   - **OBS 32:** version **1.13.0**
   - **OBS 33 or later:** version **1.14.0**. It needs OBS 33's new plugin layout, so it does not load on OBS 32.
2. In VTube Studio settings, turn on **Spout2 output**.
3. In OBS, add a **Spout2 Capture** source and select VTube Studio. Name it `VTS` or `Avatar`, so that **Parallaxa: Set up current scene** recognises it as the character.
4. **Run VTube Studio and OBS on the same GPU.** If your PC has integrated graphics as well as an NVIDIA or AMD card, Windows may start the two programs on different GPUs, and the Spout2 source stays black. Open Windows **Settings > System > Display > Graphics**, add `VTube Studio.exe` and `obs64.exe`, set both to **High performance**, then restart both programs.

## 6. Troubleshooting

| Problem | What to do |
|---|---|
| Parallaxa filters are missing in OBS | Check that OBS was closed during setup. Search the OBS log for `parallaxa`: `MODULE_INCOMPATIBLE_VER` means your OBS is older than 32.0. If OBS updated from 32 to 33 and you had unticked "Also install the copy for OBS 33 and later", run the installer again. |
| Setup says "OBS Studio is running" | Close OBS, including the tray icon, then click **Retry**. |
| The dock cannot connect | Turn on the WebSocket server in OBS, check port 4455 and enter the password again in the dock. |
| Spout2 source stays black | Put VTube Studio and OBS on the same GPU (section 5, step 4). Check the Spout2 plugin version for your OBS. |
| Windows SmartScreen warns about the installer | Unsigned test builds show this warning. Click **More info > Run anyway** only for files you downloaded from the official release page. |

## 7. Uninstall

Close OBS, then uninstall "Parallaxa (OBS plugin)" in **Settings > Apps > Installed apps**. To be safe, first choose **Tools > Parallaxa: Restore all rest poses** in OBS, so that scene items go back to their saved positions.

## 8. Licence

Parallaxa is free software under the GNU GPL v2 or later (`LICENSE.txt` in the plugin folder). `SOURCE.txt` says where the complete source code is; every release publishes `Parallaxa-<version>-source.zip` next to the binaries. Third-party components are listed in `THIRD_PARTY_NOTICES.md`.
