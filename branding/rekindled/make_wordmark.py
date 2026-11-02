"""Builds the Rekindled wordmark and lockup SVGs from stroke paths (no fonts): python make_wordmark.py
Writes rekindled-wordmark[-dark].svg and rekindled-logo[-dark].svg for the capital R wordmark ("Rekindled") and the lowercase alternative (alt/ ..., "rekindled").
The letters are 12 wide strokes with round caps, 90 tall (the ascenders of k, d, l and the capital R are the same height); the i's dot is the spark.
Spacing is 28 between letters, less after r / R: their ink is only at the top / the stem, so the gap to the next letter looks wider than it is."""
import os, re
STAR = 'M0 -1 C0.1 -0.3 0.3 -0.1 1 0 C0.3 0.1 0.1 0.3 0 1 C-0.1 0.3 -0.3 0.1 -1 0 C-0.3 -0.1 -0.1 -0.3 0 -1 Z'
GAP = {'r': 14, 'R': 16}
def letter(ch, x):
    if ch == 'R': return ['M%d 90 V0 H%d A22 22 0 0 1 %d 44 H%d M%d 44 L%d 90' % (x, x + 30, x + 30, x, x + 26, x + 54)], 54
    if ch == 'r': return ['M%d 90 V34 M%d 56 C%d 42 %d 34 %d 34' % (x, x, x, x + 10, x + 26)], 28
    if ch == 'e': return ['M%d 62 H%d A28 28 0 1 0 %d 82' % (x, x + 56, x + 48)], 56
    if ch == 'k': return ['M%d 0 V90 M%d 34 L%d 68 M%d 58 L%d 90' % (x, x + 42, x, x + 16, x + 44)], 46
    if ch == 'i': return ['M%d 34 V90' % x], 0
    if ch == 'n': return ['M%d 90 V34 M%d 56 C%d 40 %d 34 %d 34 C%d 34 %d 44 %d 58 V90' % (x, x, x, x + 14, x + 28, x + 44, x + 50, x + 50)], 50
    if ch == 'd': return ['M%d 0 V90 M%d 62 A28 28 0 1 0 %d 62.01' % (x + 56, x + 56, x + 56)], 56
    if ch == 'l': return ['M%d 0 V90' % x], 0
def build(text):
    x = 0; paths = []; spark = 0
    for i, ch in enumerate(text):
        p, w = letter(ch, x)
        paths += p
        if ch == 'i': spark = x
        x += w + (GAP.get(ch, 28) if i < len(text) - 1 else 0)
    return paths, spark, x
def word_svg_parts(text, dark):
    paths, spark, width = build(text)
    stops = ('#4D8DFF', '#8277FF', '#C26BFF') if dark else ('#1E6BFF', '#5B4DFF', '#B03CFF')
    grad = '<linearGradient id="gWord" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="%d" y2="0"><stop offset="0" stop-color="%s"/><stop offset="0.55" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % ((width,) + stops)
    body = '<g fill="none" stroke="url(#gWord)" stroke-width="12" stroke-linecap="round" stroke-linejoin="round">%s</g><g transform="translate(%d 8) scale(13 14)"><path d="%s" fill="url(#gWord)"/></g>' % (''.join('<path d="%s"/>' % p for p in paths), spark, STAR)
    return width, grad, body
mark = open('rekindled-mark.svg').read()
mark_defs = re.search(r'<defs>(.*?)</defs>', mark, re.S).group(1)
mark_body = mark.split('</defs>')[1].replace('</svg>', '').strip()
def write(text, dark, folder, suffix):
    os.makedirs(folder, exist_ok=True)
    width, grad, body = word_svg_parts(text, dark)
    open('%s/rekindled-wordmark%s.svg' % (folder, suffix), 'w', newline='\n').write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="-18 -24 %d 146" width="%d" height="146"><title>%s</title><defs>%s</defs>%s</svg>\n' % (width + 36, width + 36, text, grad, body))
    lw = 190 + width + 40
    open('%s/rekindled-logo%s.svg' % (folder, suffix), 'w', newline='\n').write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d 190" width="%d" height="190"><title>%s Viewer</title><defs>%s%s</defs><g transform="scale(0.72)">%s</g><g transform="translate(205 62)">%s</g></svg>\n' % (lw, lw, text, mark_defs, grad, mark_body, body))
for dark in (False, True):
    write('Rekindled', dark, '.', '-dark' if dark else '')
    write('rekindled', dark, 'alt-lowercase', '-dark' if dark else '')
print('written')

# ---- the lockups with the word VIEWER (heroes: the start page, the splash): a small spaced caps tagline in the same stroke style
def tag_letter(ch, x):
    if ch == 'V': return ['M%d 0 L%d 90 L%d 0' % (x, x + 30, x + 60)], 60
    if ch == 'I': return ['M%d 0 V90' % x], 0
    if ch == 'E': return ['M%d 0 H%d V90 H%d M%d 45 H%d' % (x + 46, x, x + 46, x, x + 38)], 46
    if ch == 'W': return ['M%d 0 L%d 90 L%d 24 L%d 90 L%d 0' % (x, x + 22, x + 44, x + 66, x + 88)], 88
    if ch == 'R': return ['M%d 90 V0 H%d A22 22 0 0 1 %d 44 H%d M%d 44 L%d 90' % (x, x + 30, x + 30, x, x + 26, x + 54)], 54
TAG_K = 0.30            # the tagline's letters are 27 tall against the wordmark's 90
TAG_GAPS = [70, 100, 80, 80, 100]       # V-I, I-E, E-W, W-E, E-R: less beside the diagonals (V, W), so the white between the letters looks even
def tagline(dark, extra):
    x = 0; paths = []
    for i, ch in enumerate('VIEWER'):
        p, w = tag_letter(ch, x); paths += p; x += w + (TAG_GAPS[i] + extra if i < 5 else 0)
    stops = ('#4D8DFF', '#8277FF', '#C26BFF') if dark else ('#1E6BFF', '#5B4DFF', '#B03CFF')
    grad = '<linearGradient id="gTag" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="%d" y2="0"><stop offset="0" stop-color="%s"/><stop offset="0.55" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % ((x,) + stops)
    body = '<g fill="none" stroke="url(#gTag)" stroke-width="%.1f" stroke-linecap="round" stroke-linejoin="round">%s</g>' % (5.0 / TAG_K, ''.join('<path d="%s"/>' % p for p in paths))
    return x * TAG_K, grad, body
def write_viewer_lockups(dark):
    suffix = '-dark' if dark else ''
    ww, wgrad, wbody = word_svg_parts('Rekindled', dark)
    # horizontal: the mark, then the wordmark with VIEWER under it, left aligned
    tw, tgrad, tbody = tagline(dark, 0)
    lw = 190 + ww + 40
    open('rekindled-logo-viewer%s.svg' % suffix, 'w', newline='\n').write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d 190" width="%d" height="190"><title>Rekindled Viewer</title><defs>%s%s%s</defs><g transform="scale(0.72)">%s</g><g transform="translate(205 30)">%s</g><g transform="translate(205 157) scale(%s)">%s</g></svg>\n' % (lw, lw, mark_defs, wgrad, tgrad, mark_body, wbody, TAG_K, tbody))
    # stacked: the mark over the wordmark over VIEWER, centred
    tw2, tgrad2, tbody2 = tagline(dark, 45)
    W = ww + 120; mark_px = 0.8 * 256
    H = int(mark_px + 36 + 90 + 34 + 27 + 40)
    mx = (W - mark_px) / 2
    open('rekindled-logo-stacked%s.svg' % suffix, 'w', newline='\n').write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><title>Rekindled Viewer</title><defs>%s%s%s</defs><g transform="translate(%.1f 8) scale(0.8)">%s</g><g transform="translate(%.1f %d)">%s</g><g transform="translate(%.1f %d) scale(%s)">%s</g></svg>\n' % (W, H, W, H, mark_defs, wgrad, tgrad2, mx, mark_body, (W - ww) / 2, int(mark_px + 36), wbody, (W - tw2) / 2, int(mark_px + 36 + 90 + 34), TAG_K, tbody2))
for dark in (False, True):
    write_viewer_lockups(dark)
print('viewer lockups written')
