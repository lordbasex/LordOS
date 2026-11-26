# LordOS wallpapers

Created by iPERFEX with an AI image generator; part of the LordOS brand assets.

The collection that LordOS ships in `/usr/share/backgrounds/lordos/` and shows in
**Settings → Wallpaper**, grouped by category. The default theme also keeps its four gradients
(Aurora, Ocean, Sunset, Graphite; `themes/default/assets/`, made by `tools/gen-wallpaper.py`) as the
"Gradients" category.

| Category | File | Name (en / es / pt) |
|---|---|---|
| Signature | `signature/lordos-crystal-crown.jpg` | Crystal Crown / Corona de cristal / Coroa de cristal (**login screen**) |
| Signature | `signature/lordos-eternal-dawn.jpg` | Eternal Dawn / Amanecer eterno / Amanhecer eterno |
| Signature | `signature/lordos-royal-flow.jpg` | Royal Flow / Flujo real / Fluxo real |
| Signature | `signature/lordos-royal-waves.jpg` | Royal Waves / Ondas reales / Ondas reais |
| Signature | `signature/lordos-violet-horizon.jpg` | Violet Horizon / Horizonte violeta / Horizonte violeta |
| Lumo | `lumo/lordos-lumo-aurora.jpg` | Aurora |
| Lumo | `lumo/lordos-lumo-celestial.jpg` | Celestial (**default desktop** for new users) |
| Cosmos | `cosmos/lordos-event-horizon.jpg` | Event Horizon / Horizonte de sucesos / Horizonte de eventos |
| Cosmos | `cosmos/lordos-infinite-galaxy.jpg` | Infinite Galaxy / Galaxia infinita / Galáxia infinita |
| Cosmos | `cosmos/lordos-solar-odyssey.jpg` | Solar Odyssey / Odisea solar / Odisseia solar |
| Cosmos | `cosmos/lordos-stellar-voyage.jpg` | Stellar Voyage / Viaje estelar / Viagem estelar |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-panorama.jpg` | Panorama (**default desktop in Workspaces**) |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-neon-road-entrance.jpg` | Neon Road Entrance / Entrada de neón / Entrada de neon |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-heart-of-the-city.jpg` | Heart of the City / Rumbo al corazón de la ciudad / Rumo ao coração da cidade |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-skyline-highway.jpg` | Skyline Highway / Autopista panorámica / Rodovia panorâmica |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-rings-and-bridges.jpg` | Rings and Bridges / Anillos y puentes / Anéis e pontes |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-flower-boulevard.jpg` | Flower Boulevard / Bulevar florido / Bulevar florido |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-neon-cherry-avenue.jpg` | Neon Cherry Avenue / Avenida de cerezos de neón / Avenida das cerejeiras de neon |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-maglev-station.jpg` | Maglev Station / Estación maglev / Estação maglev |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-pier-and-train.jpg` | Pier and Train / Muelle y tren / Cais e trem |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-sky-terrace.jpg` | Sky Terrace / Terraza en las alturas / Terraço nas alturas |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-metropolis-lookout.jpg` | Metropolis Lookout / Mirador sobre la metrópolis / Mirante sobre a metrópole |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-hover-rider.jpg` | Neon Rider / Jinete de neón / Piloto de neon |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-terrace-woman.jpg` | Night Watch / Vigía nocturna / Vigia noturna |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-hooded.jpg` | Hooded / Encapuchada / Encapuzada |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-unmasked.jpg` | Unmasked / Sin máscara / Sem máscara |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-fashion.jpg` | Night Pose / Pose nocturna / Pose noturna |
| Cyberpunk City HD | `cyberpunk/lordos-cyberpunk-portrait.jpg` | Portrait / Retrato / Retrato |

The names shown in the interface come from `assets/locales/<lang>/shell.json`
(`shell.wallpaper.<id>`) and the categories from `settings.json` (`settings.wallpaperCategory.*`). "Cyberpunk City HD"
is a proper name and stays the same in every language, like Lumo.

## Files

- `<category>/lordos-<name>.jpg`: the wallpaper at the original's native size (1672×941 today),
  JPEG quality 90, 4:4:4 chroma, baseline. About 4 MB for all eleven; the 2 MB PNG originals are
  not kept in the repository (the JPEGs measure 39–46 dB PSNR against them and look the same).
- **Cyberpunk City HD** (`cyberpunk/`, 0.8.9): seventeen pictures the maintainer made for LordOS (no text or marks in
  them), listed in the order of a journey into the city (`order.txt` next to the originals). Panorama and Neon Road
  Entrance are 3840×2160: a 1920×1080 base (Lanczos, quality 92, 4:4:4) and the original as the `@3840x2160` variant
  (quality 95, 4:4:4: 4:2:0 at 80 visibly softened the neon edges). The other fifteen are 1672×941 originals brought
  up to 1920×1080 once (Lanczos, quality 92, 4:4:4; `fill` in the generator's ENCODE), with no 4K file. 22.4 MB with
  the thumbnails. The source files are not kept here.
- `thumbs/<category>/lordos-<name>.jpg`: 320×180 thumbnails for Settings and the Control Center,
  so the pickers never decode the full images.
- `index.json`: the list (ids, categories, sizes) that the SDK bundles (`sdk/wallpapers.js`).

Everything here is generated by `node tools/gen-wallpapers.mjs <folder with the originals>`
(ffmpeg and cjpeg; PNG or JPEG originals in `signature/`, `lumo/`, `cosmos/`, `cyberpunk/`). A category missing
from the folder keeps its entries in `index.json`, so one category can be regenerated alone. Don't edit the files by
hand.

## The slideshow

Workspaces can go through a category's pictures on its own (Settings → Wallpaper, "Change the wallpaper
automatically"; `sdk/slideshow.jsx`, docs/WORKSPACES.md "Wallpaper slideshow"). It decodes each picture off the
interface's thread at the size it is drawn at (`thumb:<px>:asset:wallpapers/…`): the display plus 2 % on each side for
the transition's pan, so a 1080p display decodes an `@3840x2160` file down to 1999×1124. The still wallpaper of a
1080p display keeps using the 1920×1080 base.

## Higher resolutions

The originals are 1672×941, so they are upscaled (bilinear, "cover" crop) on bigger screens: fine on
1280×800 and 1920×1080, a little soft on 2560×1440 and visibly soft on 4K. To add a sharper version,
put the PNG next to the original with its size in the name and run the generator again:

```
<originals>/lumo/lordos-lumo-celestial@3840x2160.png  →  lumo/lordos-lumo-celestial@3840x2160.jpg
```

The desktop and the login screen then pick, for each screen, the smallest variant that covers it
(`wallpaperSrc()` in `sdk/wallpapers.js`), so a 1080p screen keeps the light base file and a 4K
screen gets the 4K one. No code changes are needed.

## License and provenance

The images were created by iPERFEX with an AI image generator and are part of the LordOS brand
assets, like the logo and Lumo. Cyberpunk City HD was made for LordOS by its maintainer and is distributed
under the same terms. The files are distributed with LordOS under the same terms as the
rest of the repository (MIT); the LordOS name, the crown and Lumo that appear in them are also
covered by the LordOS trademark and brand policy (`web/content/legal/trademark.en.md`). Because
they are AI-generated, copyright protection for the images themselves may be limited in some
countries; the trademark policy still applies to the brand elements in them. The "Lumo" pieces are
an artistic interpretation of the celestial fox, not the official mascot drawing (for that, use
`assets/brand/mascot/`).
