# LordOS Studio's test frames (build/studio-nn-test, build/studio-test)

Ten photographs from Wikimedia Commons, each released by its author under **CC0 1.0** (public domain dedication), as
the file page says (checked 2026-11-22). Each was cropped to 16:9 and scaled with ffmpeg's area filter to 256×144
(`fNN-seg.png`, the Selfie Segmenter's input); the centre 144×144 of that, scaled to 128×128, is `fNN-face.png`
(BlazeFace's input). Our crops are CC0 too.

| Frames | Photograph | Author (Commons user) | License | Page |
|---|---|---|---|---|
| `f00-*` | Diego de la Hera en enero de 2023.jpg | Diegodlh | CC0 | https://commons.wikimedia.org/wiki/File:Diego_de_la_Hera_en_enero_de_2023.jpg |
| `f01-*` | A Selfie Image of Kate Dimitrova.jpg | DrMedRenArtHistory | CC0 | https://commons.wikimedia.org/wiki/File:A_Selfie_Image_of_Kate_Dimitrova.jpg |
| `f02-*` | Selfie image of Cecile McKee.jpg | Cecilemckee | CC0 | https://commons.wikimedia.org/wiki/File:Selfie_image_of_Cecile_McKee.jpg |
| `f03-*` | Karthik bharamagiri.jpg | Harihn08980 | CC0 | https://commons.wikimedia.org/wiki/File:Karthik_bharamagiri.jpg |
| `f04-*` | Smiling older woman in a patterned headscarf in Santa Lucía, Maracaibo, Venezuela.jpg | Wilfredor | CC0 | https://commons.wikimedia.org/wiki/File:Smiling_older_woman_in_a_patterned_headscarf_in_Santa_Luc%C3%ADa,_Maracaibo,_Venezuela.jpg |
| `f05-*` | Woman in a headwrap in Quebec City.jpg | Wilfredor | CC0 | https://commons.wikimedia.org/wiki/File:Woman_in_a_headwrap_in_Quebec_City.jpg |
| `f06-*` | Larry-buttrose-selfie-sep-2025.jpg | LarryButtrose | CC0 | https://commons.wikimedia.org/wiki/File:Larry-buttrose-selfie-sep-2025.jpg |
| `f08-*` | AdamKwasman.jpg | BMRoosevelt | CC0 | https://commons.wikimedia.org/wiki/File:AdamKwasman.jpg |
| `f09-*` | Young women taking a selfie, Place du Château, Strasbourg, 2025.jpg | DimiTalen | CC0 | https://commons.wikimedia.org/wiki/File:Young_women_taking_a_selfie,_Place_du_Ch%C3%A2teau,_Strasbourg,_2025.jpg |
| `f10-*` | Jonny McGovern.jpg | Jonny McGovern | CC0 | https://commons.wikimedia.org/wiki/File:Jonny_McGovern.jpg |

`fNN-seg.ref` and `fNN-face.ref` are ONNX Runtime 1.21's outputs for those frames (one thread, on the dev machine),
written by `tools/studio/ort-ref.c` from the ONNX graphs `tools/studio/tflite2lnn.mjs --onnx` makes: the segmenter's
144×256 probabilities as little-endian u16 (value × 65535), BlazeFace's 896×16 regressors then 896 scores as
little-endian float16. LordOS never links ONNX Runtime; it is only the reference, the way `tests/hb_compare.py` uses
HarfBuzz.
