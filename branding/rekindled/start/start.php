<?php
// The Rekindled start page, shown in the viewer's login screen (https://sl-op.com/rekindled/start.php).
// The viewer adds its own version number to the address (?v=0.99.01.1381). The page shows it under the logo and, when
// the viewer is older than the newest release in /releases, offers the download page. No version in the address means
// an older viewer (or a plain browser), so the button is shown. Older viewers load start.html, a static page that always
// shows the button. Nothing is stored or logged here, and any failure falls back to the plain logo page.
error_reporting(0);
ini_set('display_errors', '0');
header('Content-Type: text/html; charset=utf-8');
header('Cache-Control: no-store');

function parse_version($s)
{
    if (!is_string($s) || !preg_match('/^(\d{1,4})\.(\d{1,4})\.(\d{1,4})\.(\d{1,6})$/', $s, $m)) {
        return null;
    }
    return [(int)$m[1], (int)$m[2], (int)$m[3], (int)$m[4]];
}

function compare_versions($a, $b)
{
    for ($i = 0; $i < 4; $i++) {
        if ($a[$i] != $b[$i]) {
            return $a[$i] < $b[$i] ? -1 : 1;
        }
    }
    return 0;
}

// the highest version among the Windows installers in /releases (Rekindled-Beta_AVX2-0-99-01-1381_Setup.exe)
function newest_release()
{
    $best = null;
    foreach ((array)glob(__DIR__ . '/../releases/Rekindled-Beta_AVX2-*_Setup.exe') as $file) {
        if (preg_match('/Rekindled-Beta_AVX2-(\d+)-(\d+)-(\d+)-(\d+)_Setup\.exe$/', $file, $m)) {
            $v = [(int)$m[1], (int)$m[2], (int)$m[3], (int)$m[4]];
            if ($best === null || compare_versions($v, $best) > 0) {
                $best = $v;
            }
        }
    }
    return $best;
}

$shown = null;      // the version text to show (digits and dots only)
$older = false;     // the viewer is older than the newest release
try {
    $raw = isset($_GET['v']) ? $_GET['v'] : '';
    $mine = parse_version($raw);
    $shown = $mine ? $raw : null;
    $newest = newest_release();
    if ($mine === null) {
        $older = true;      // no (valid) version in the address: an older viewer
    } elseif ($newest !== null) {
        $older = compare_versions($mine, $newest) < 0;
    }
} catch (Throwable $e) {
    $shown = null;
    $older = false;
}
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rekindled Viewer</title>
<style>
  :root {
    --bg-top: #070b1f;
    --bg-bottom: #14103f;
    --text: #aab2d8;
    --text-strong: #dfe3ff;
    --text-dim: #7e86b0;
    --link: #8f9bff;
    --rule: rgba(143, 155, 255, 0.18);
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body {
    background: linear-gradient(180deg, var(--bg-top), var(--bg-bottom)) fixed;
    color: var(--text);
    font: 14px/1.5 "Segoe UI", system-ui, -apple-system, Roboto, Arial, sans-serif;
    display: flex;
    flex-direction: column;
    min-height: 100%;
  }
  main {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 32px 16px;
  }
  main img { width: min(640px, 86vw); height: auto; display: block; }
  .ver { margin: 14px 0 0; font-size: 15px; color: var(--text-dim); letter-spacing: 0.02em; }
  a.btn {
    display: inline-block;
    margin-top: 14px;
    padding: 10px 22px;
    border-radius: 12px;
    border: 1px solid var(--link);
    background: rgba(143, 155, 255, 0.14);
    color: var(--text-strong);
    font-size: 15px;
    font-weight: 600;
    text-align: center;
    text-decoration: none;
  }
  a.btn:hover { background: rgba(143, 155, 255, 0.26); }
  footer {
    border-top: 1px solid var(--rule);
    padding: 20px 16px 24px;
    text-align: center;
    color: var(--text-dim);
    font-size: 13px;
  }
  footer a { color: var(--link); text-decoration: none; }
  footer a:hover { text-decoration: underline; }
  .credits {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: center;
    gap: 14px 36px;
    margin-top: 8px;
  }
  .powered { margin-top: 18px; font-size: 12px; letter-spacing: 0.08em; text-transform: uppercase; }
  .credits a { display: inline-flex; align-items: center; }
  .credits img { height: 32px; width: auto; display: block; opacity: 0.9; }
  .credits a:hover img { opacity: 1; }
  /* the Vulkan and OpenAL logos are dark red, so they sit on a light chip to stay legible on the dark page */
  .credits a.chip { background: #eef0ff; border-radius: 8px; padding: 6px 14px; }
  .credits a.chip img { height: 24px; opacity: 1; }
  .credits a.chip img.openal { height: 34px; }
</style>
</head>
<body>
<main>
  <img src="/rekindled/rekindled-logo.svg" alt="Rekindled Viewer">
<?php if ($shown !== null): ?>
  <p class="ver">Version <?php echo htmlspecialchars($shown, ENT_QUOTES, 'UTF-8'); ?></p>
<?php endif; ?>
<?php if ($older): ?>
  <a class="btn" href="https://sl-op.com/rekindled/" target="_blank" rel="noopener">Get the Latest Version</a>
<?php endif; ?>
</main>
<footer>
  <div>Based on <a href="https://www.firestormviewer.org/" target="_blank" rel="noopener">Firestorm Viewer</a> &middot; <a href="https://sl-op.com/rekindled/privacy" target="_blank" rel="noopener">Privacy Policy</a></div>
  <div class="powered">Powered by</div>
  <div class="credits">
    <a class="chip" href="https://openal-soft.org/" target="_blank" rel="noopener" title="OpenAL Soft"><img class="openal" src="/rekindled/openal-logo.png" alt="OpenAL"></a>
    <a class="chip" href="https://vulkan.org/" target="_blank" rel="noopener" title="Vulkan"><img src="/rekindled/vulkan-logo.svg" alt="Vulkan"></a>
  </div>
</footer>
</body>
</html>
