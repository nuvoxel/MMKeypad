# `CURRENT_MEDIA_INFO` (room var 1031) — verified schema

> Captured live 2026-07-22 from the Office (room 2434) playing Apple Music via the SA1.
> Resolves the CLAUDE.md flag "exact schema inside CURRENT_MEDIA_INFO (var 1031) unverified."
> This is the room-variable form of now-playing; the MediaSession event
> (`OnMediaSessionMediaInfoChanged`, Stage 1) carries the same fields as a structured push.

## Full example (real capture, artwork/DRM blobs truncated)

```xml
<mediainfo>
  <deviceid>100002</deviceid>          <!-- session owner = Digital Media agent, NOT the source -->
  <queueid>10000</queueid>
  <streamid>0</streamid>
  <source>2</source>
  <mediatype>SONG</mediatype>
  <mediatypeV2>GENERIC_MEDIA</mediatypeV2>
  <medSrcDev>2687</medSrcDev>           <!-- the REAL source device (Music A = media_service) -->
  <queueInfo>1</queueInfo>
  <streamStatus>audioformat=AAC decoder,audioquality=Radio,bitdepth=16,bitlayout=4,
                bitrate=250766,channels=2,drmstatus=<base64>,samplerate=48000,
                status=OK_playing</streamStatus>
  <stationid>drm+applemusic://<base64 auth blob></stationid>
  <album>Life of a Wallflower , Vol. 2</album>
  <artist>Elley Duhé &amp; Whethan</artist>
  <title>MONEY ON THE DASH</title>
  <channel/>
  <meta>
    <audioFormat>AAC decoder</audioFormat>
    <sampleRate>48 kHz</sampleRate>
    <bitDepth>16</bitDepth>
    <channels>2</channels>
    <bitRate>250766</bitRate>
    <audioQuality>Radio</audioQuality>
  </meta>
  <img>aHR0cHM6…base64…</img>            <!-- base64-encoded artwork URL -->
</mediainfo>
```

## Field notes

| Field | Meaning / gotcha |
|---|---|
| `deviceid` | Session **owner** device (Digital Media agent 100002), NOT the source. |
| `medSrcDev` | The actual source device (e.g. 2687 "Music A" media_service). Use THIS to identify the source. |
| `source` | Source index within the room (small int), not a device id. |
| `mediatype` | Coarse: `SONG` / … |
| `mediatypeV2` | `GENERIC_MEDIA` here — coarse, weak isRadio signal (interim only). |
| `title` / `artist` / `album` | Plain text, XML-escaped (`&amp;`). Present. |
| `img` | **base64-encoded HTTPS URL** to artwork. Decode → e.g. `https://is1-ssl.mzstatic.com/…/1024x1024bb.jpg`. Not raw image bytes. |
| `stationid` | `drm+applemusic://` + base64 auth blob — opaque, source-specific handle. |
| `streamStatus` | CSV: `status=OK_playing` (play-state signal), `samplerate`, `bitrate`, `drmstatus=<base64>`. |
| `meta` | Human-formatted duplicates of stream fields (`48 kHz`, etc.) for display. |
| `channel` | Empty for streaming; populated for broadcast/tuner. |

**For the keypad display:** `title`, `artist`, `album`, and `img` (base64-decode → fetch
URL) are the render fields. `status=OK_playing` in `streamStatus` = play/pause state.
`medSrcDev` identifies which source to attribute. No shuffle/repeat here — those come from
var 1006 (`QUEUE_STATUS_V2`); see [control4-media-commands.md](media-commands.md).
