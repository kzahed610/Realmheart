# Realmheart icon color tokens

Updated SVGs use semantic classes and custom properties:

- `.rh-icon-primary` / `--rh-icon-primary`: normal icon foreground
- `.rh-icon-accent` / `--rh-icon-accent`: charging bolt or warning badge
- `.rh-icon-on-accent` / `--rh-icon-on-accent`: mark drawn on an accent badge

Suggested values:

Dark mode:
- primary: `#F5F2EA`
- accent: `#FFD66B`
- on-accent: `#18151F`

Light mode:
- primary: `#17141D`
- accent: `#6D42D8`
- on-accent: `#FFFFFF`

If Realmheart processes SVG source before rendering, replace the `var(...)` tokens with the resolved theme colors. If the SVG is inlined, set the custom properties on the root SVG or a parent element.
