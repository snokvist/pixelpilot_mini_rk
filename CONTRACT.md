# External OSD Protocol

PixelPilot Mini RK supports an external OSD control feed over UDP. This allows external applications to push text, values, and zoom commands to the OSD in real-time.

## Transport

*   **Protocol:** UDP
*   **Default Port:** 5005 (configurable via `--osd-external-udp-port`)
*   **Payload:** JSON string

## JSON Payload Structure

The payload is a JSON object. All fields are optional, but at least one should be present to have an effect.

```json
{
  "texts": ["string1", "string2", ...],
  "values": [1.23, 4.56, ...],
  "zoom": "200,200,50,50",
  "asset_updates": [{"id": 0, "enabled": false}],
  "ttl_ms": 1000
}
```

### Fields

| Field | Type | Description |
| :--- | :--- | :--- |
| `texts` | Array of Strings | Updates the text slots. Max 8 items. Strings are mapped to slots 0-7 by index. |
| `values` | Array of Numbers | Updates the value slots. Max 8 items. Values are mapped to slots 0-7 by index. |
| `zoom` | String | Sets the zoom level and center point. See "Zoom Control" below. |
| `ttl_ms` | Integer | Time-to-live in milliseconds. If present, the updated slots will expire and clear after this duration. If omitted, updates are persistent until overwritten or cleared. |
| `asset_updates` | Array of Objects | Optional compatibility field matching `waybeam_osd`. Each object supports `id` (0-7) and `enabled` (boolean) to show/hide a configured OSD element by configured asset `id`. |

## Slot Mapping

The OSD engine maintains 8 text slots and 8 value slots (indices 0-7).
*   Sending `texts` updates slots starting from index 0.
*   Sending `values` updates slots starting from index 0.
*   Sending an empty array `[]` for `texts` or `values` clears all respective slots.
*   Sending an empty string `""` for a specific text slot clears that slot.
*   Sending `null` for a specific slot skips the update for that slot, preserving its current value. This allows multiple senders to update different indices without interference.

To reference these slots in your `osd.ini` configuration, use tokens like `{ext.text1}`, `{ext.value1}`, etc.

## Zoom Control

Zoom can be controlled by sending a string in the `zoom` field.

*   **Command Format:** `SCALE_X,SCALE_Y,CENTER_X,CENTER_Y` (or `zoom=SCALE_X,SCALE_Y,CENTER_X,CENTER_Y` for backward compatibility)
    *   `SCALE_X`, `SCALE_Y`: Zoom percentage (e.g., 100 is 1x, 200 is 2x).
    *   `CENTER_X`, `CENTER_Y`: Center point percentage (0-100).
*   **Disable Zoom:** `off` (or `zoom=off`)

**Example:**
```json
{
  "zoom": "200,200,50,50"
}
```
This sets 2x zoom centered on the middle of the screen.

## Examples

**1. Display a message for 5 seconds:**
```json
{
  "texts": ["Hello World"],
  "ttl_ms": 5000
}
```

**2. Update battery voltage and status:**
```json
{
  "texts": ["Battery OK"],
  "values": [12.4]
}
```

**3. Clear all external text and values:**
```json
{
  "texts": [],
  "values": []
}
```

## Asset Visibility Control (waybeam_osd compatible)

To match `waybeam_osd` and reuse `osd_send.c`, send `asset_updates` entries with `id` and `enabled`.

* `id` maps to the configured OSD element `id` (0-7) set in `[osd.element.*].id`.
* `enabled: false` hides the element immediately.
* `enabled: true` shows the element again.
* Unknown fields in each object are ignored.
* If `ttl_ms` is supplied, the visibility override expires automatically after the TTL.

**Example:**
```json
{
  "asset_updates": [
    {"id": 3, "enabled": false},
    {"id": 4, "enabled": true}
  ]
}
```
