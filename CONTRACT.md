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
  "text": ["string1", "string2", ...],
  "value": [1.23, 4.56, ...],
  "ttl_ms": 1000
}
```

### Fields

| Field | Type | Description |
| :--- | :--- | :--- |
| `text` | Array of Strings | Updates the text slots. Max 8 items. Strings are mapped to slots 0-7 by index. |
| `value` | Array of Numbers | Updates the value slots. Max 8 items. Values are mapped to slots 0-7 by index. |
| `ttl_ms` | Integer | Time-to-live in milliseconds. If present, the updated slots will expire and clear after this duration. If omitted, updates are persistent until overwritten or cleared. |

## Slot Mapping

The OSD engine maintains 8 text slots and 8 value slots (indices 0-7).
*   Sending `text` updates slots starting from index 0.
*   Sending `value` updates slots starting from index 0.
*   Sending an empty array `[]` for `text` or `value` clears all respective slots.
*   Sending an empty string `""` for a specific text slot clears that slot.

To reference these slots in your `osd.ini` configuration, use tokens like `{external.text0}`, `{external.value0}`, etc.

## Zoom Control

The last text slot (index 7) is reserved for Zoom commands.

*   **Command Format:** `zoom=SCALE_X,SCALE_Y,CENTER_X,CENTER_Y`
    *   `SCALE_X`, `SCALE_Y`: Zoom percentage (e.g., 100 is 1x, 200 is 2x).
    *   `CENTER_X`, `CENTER_Y`: Center point percentage (0-100).
*   **Disable Zoom:** `zoom=off`

**Example:**
```json
{
  "text": ["", "", "", "", "", "", "", "zoom=200,200,50,50"]
}
```
This sets 2x zoom centered on the middle of the screen.

## Examples

**1. Display a message for 5 seconds:**
```json
{
  "text": ["Hello World"],
  "ttl_ms": 5000
}
```

**2. Update battery voltage and status:**
```json
{
  "text": ["Battery OK"],
  "value": [12.4]
}
```

**3. Clear all external text and values:**
```json
{
  "text": [],
  "value": []
}
```
