# Font Awesome toolbar sources

Font Awesome Free 6.7.2 by Fonticons, Inc. (https://fontawesome.com).
SVG sources: https://github.com/FortAwesome/Font-Awesome/tree/6.7.2/svgs

These unmodified SVGs are licensed under CC BY 4.0; see LICENSE.txt.
Summit's `tools/make-toolbar-icons.py` converts them to Haiku vector icons,
centers and optically sizes them on a 20-point canvas. At runtime Summit
recolors the silhouettes to match the theme and control state. No font is
installed or loaded. The mapping and sizes are in that script's ICONS table.
