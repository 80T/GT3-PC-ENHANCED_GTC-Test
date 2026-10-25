# Units

The game's units (`UnitSettings`, T518) and what GT3 PC does with them
(`src/game/app/pc_units.*`; the settings `[Game] SpeedUnit`, `PowerUnit`,
`TorqueUnit`: the launcher's Game tab, F2 GAME > SPEED / POWER / TORQUE
UNIT).

## What the code has

Internally a speed is m/s, a distance metres (the odometer 2 m units), a
power PS, a torque kgf.m (the database's hundredths, the spec sheet's
tenths). `UnitSettings` converts for the screen:

| | units | factor from the internal one |
|---|---|---|
| speed / distance (`system`) | metric km/h, km, m; imperial mph, miles | x 3.6; x 3.6 x 0.62139 |
| power | PS, HP, BHP, CV, kW | 1; 0.98619; 0.98619; 1 (PC; retail 0.98619); 0.7355 |
| torque | kgf.m, ft.lb, Nm | 1; 7.2329; 9.8 |

and a preset per country (`set_units_for_country`, T050), all in the US
code: Japanese km/h PS kgf.m, US English mph HP ft.lb, UK English / Italian
/ Spanish km/h BHP kgf.m, French km/h CV kgf.m, German km/h PS kgf.m. The US
build only ever ran its own.

## The settings

`auto` (the language's preset) or a unit, for each of the three. Two sets
(`pc_units.h`):

- `g_002F59D8`, the units shown: the preset with the settings over it -
  every text that prints a unit's name (`speedName`, `powerName`, ...);
- `g_pcGameUnits`, the preset alone: the game's own figures whose unit is
  not printed - the power-to-weight ratios that choose the arcade race class
  (T026: S below 2.1 kg per power unit ...), order a race's grid (T102,
  T151) and go into a ranking code (T102), and the menus' ratios shown next
  to a unit drawn in their art (T036 `spec_p_w_ratio*`, `spec_t_w_ratio*`).
  Retail computed all of them in the shown units: a kW choice would have
  changed races (power x 0.7355).

A change applies at the next tick (`pc_units_sync`, `pc_frame_tick_begin`);
a race's speedometer dial is chosen at its start.

## Bugs fixed (2026-10-24)

- **CV** took HP's factor (0.98619); the cheval-vapeur is the metric
  horsepower, the PS itself: French power figures were 1.4 % low.
- **The computed torque** (`ShowRoomCar::torque`, the garage's "torque"
  texts) went through `convPower`: ft.lb and Nm showed the kgf.m figure
  (x 0.98619) under their name.
- **Saved thumbnails' power** (the memory card screens: a replay's, a best
  lap's, a setting's - T129, T488) were stored converted and shown
  converted again (T069); now stored in PS. (Earlier saves' thumbnails show
  1.4 % low with HP.)
- **The total mileage** (`status_ttl_milage`) divided by 50 (tenths of a
  km) whatever the unit: the US build showed km as miles. Now / 80 for
  tenths of a mile, as the odometer.
- **The licence launch speed** (`make_license_launch_speed`) printed the
  race database's km/h figure with the shown unit's name ("80mph" for 80
  km/h). Now converted.
- **The HUD trip meter** took 0.625 miles per km; 0.621371 now.
- **The torque-to-weight ratios** (`spec_t_w_ratio*`) were a hundredth of
  the ratio (weight / tenths x 0.1); weight x 10 / tenths now.
- **The data logger** drew miles and mph whatever the unit (the 1/64-mile
  grid, " <n>ml" labels, the mph speed lines and labels, the cursor's
  "ml" / "mph" readouts); metric now has a 25 m grid, a label each km, km/h
  lines, labels and readouts.
- **The speedometer dial**: mph shown but no mph dial in the display images
  (the Japanese disc's `race/JP/display.imgs` has none) - the dial and its
  needle stay km/h; the digital speed follows the setting (with the frame
  blending too: it shows the needle's blended value, converted back from
  km/h).
- The torque name "NM" is "Nm".

Checked and right: the odometer (/ 80 for miles), the arcade course length
(metres x 0.62139: thousandths of a mile), the speedometer scale (steps of
50 mph / 80 km/h), the HUD and event speeds (converted once: the speed event
at its post, shown as is), the machine test's records (kept in m/s; the
comment calling them km/h is wrong), every power / torque text with its
unit's name, the car spec getters (`getMaxPower` / `getMaxTorque`). Not
changeable: units drawn in the menus' pictures, and the weight (kg only: the
code has no weight unit).
