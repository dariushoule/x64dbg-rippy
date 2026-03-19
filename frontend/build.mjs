import * as esbuild from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync } from 'fs';
import { dirname, resolve } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const watch = process.argv.includes('--watch');


// Build JS + CSS bundles to memory via write:false, then inline into HTML
const jsxOptions = {
    jsx: 'transform',
    jsxFactory: 'h',
    jsxFragment: 'Fragment',
};

const result = await esbuild.build({
    entryPoints: [resolve(__dirname, 'src/index.jsx')],
    bundle: true,
    minify: !watch,
    write: false,
    outdir: 'out',
    loader: { '.css': 'css' },
    ...jsxOptions,
});

// Separate JS and CSS outputs
let js = '';
let css = '';
for (const file of result.outputFiles) {
    if (file.path.endsWith('.js')) js = file.text;
    if (file.path.endsWith('.css')) css = file.text;
}

// Read HTML template and inject bundles
let html = readFileSync(resolve(__dirname, 'index.html'), 'utf8');
html = html.replace('<!-- __CSS__ -->', () => `<style>${css}</style>`);
html = html.replace('<!-- __JS__ -->', () => `<script>${js}</script>`);

// Write to src/chat_ui.html (picked up by plugin.rc)
const outPath = resolve(__dirname, '..', 'src', 'chat_ui.html');
mkdirSync(dirname(outPath), { recursive: true });
writeFileSync(outPath, html);

console.log(`Built: ${outPath} (${html.length} bytes)`);

if (watch) {
    // In watch mode, rebuild on changes
    const ctx = await esbuild.context({
        entryPoints: [resolve(__dirname, 'src/index.jsx')],
        bundle: true,
        minify: false,
        write: false,
        outdir: 'out',
        loader: { '.css': 'css' },
        ...jsxOptions,
        plugins: [{
            name: 'rebuild-html',
            setup(build) {
                build.onEnd(result => {
                    if (result.errors.length > 0) return;
                    let js = '', css = '';
                    for (const file of result.outputFiles) {
                        if (file.path.endsWith('.js')) js = file.text;
                        if (file.path.endsWith('.css')) css = file.text;
                    }
                    let html = readFileSync(resolve(__dirname, 'index.html'), 'utf8');
                    html = html.replace('<!-- __CSS__ -->', () => `<style>${css}</style>`);
                    html = html.replace('<!-- __JS__ -->', () => `<script>${js}</script>`);
                    writeFileSync(outPath, html);
                    console.log(`Rebuilt: ${outPath} (${html.length} bytes)`);
                });
            }
        }],
    });
    await ctx.watch();
    console.log('Watching for changes...');
}
