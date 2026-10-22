# Off-grid mode

Off-grid mode makes a companion forward packets for others, like a small repeater. It is for a group
of handhelds that is out of reach of any repeater: each node relays for the rest, so messages get
further than one radio hop.

It is MeshCore's "client repeat". ZephCore nodes and stock MeshCore nodes in off-grid mode work
together.

## It runs on its own frequencies

A companion forwards only on one of three frequencies set aside for this:

| Band | Off-grid frequency |
|---|---|
| 433 MHz | 433.000 MHz |
| EU 868 MHz | 869.495 MHz |
| US 915 MHz | 918.000 MHz |

Turning off-grid mode on therefore **moves the node off your normal mesh**. While it is on you reach
only other nodes on the same off-grid frequency with the same bandwidth, spreading factor and coding
rate. Those three, and the TX power, are left as they were, so a group that switches from the same
mesh preset ends up on matching settings.

A companion never forwards on a normal mesh frequency. It forwards everything it hears, without the
hop limits, region filters and loop detection a repeater applies, which is fine for a handful of
handhelds and harmful on a busy mesh.

## Turning it on and off

**From the app.** Choose one of the off-grid frequencies in the radio settings and enable repeat.
When you turn repeat off in the app the node stays on that frequency: select your mesh preset again
to return.

**From the display.** The Offgrid page (button boards) or System > Device > Offgrid (joystick boards)
asks for confirmation, then moves to the off-grid frequency nearest to the one you are on and starts
forwarding. Turning it off there returns to the frequency you came from. If the node was already on an
off-grid frequency when you turned it on, it stays there.

**From the CLI** (USB or the app's command line, not over the mesh): `set repeat on` and
`set repeat off` do the same as the display, and `get repeat` shows the state.

On a 2.4 GHz preset there is no off-grid frequency, and the display and the CLI refuse.

## After an update from 1.17.7 or earlier

Up to 1.17.7 the display toggle turned forwarding on without changing the frequency, so a companion
could end up repeating on the live mesh. A node left in that state stops forwarding when it boots
1.18.0 or later, and shows off-grid mode as off.
