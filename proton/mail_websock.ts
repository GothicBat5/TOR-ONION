import HtmlWebpackPlugin from 'html-webpack-plugin';
import { produce, setAutoFreeze } from 'immer';
import path from 'path';
import type { Configuration } from 'webpack';
import { ProvidePlugin } from 'webpack';

import { type WebpackEnvArguments, getWebpackOptions } from '@proton/pack/lib/config';
import { addDevEntry, getConfig } from '@proton/pack/webpack.config';
import { getIndexChunks, getSupportedEntry, mergeEntry } from '@proton/pack/webpack/entries';

import appConfig from './appConfig';

const result = (opts: WebpackEnvArguments): Configuration => {
    const webpackOptions = getWebpackOptions(opts, { appConfig });
    const config = getConfig(webpackOptions);

    setAutoFreeze(false);

    return produce(config, (config) => {
        config.plugins = config.plugins || [];
        config.resolve = config.resolve || {};
        config.resolve.fallback = config.resolve.fallback || {};
        config.resolve.fallback.buffer = require.resolve('buffer');
        config.plugins.push(
            new ProvidePlugin({
                Buffer: [require.resolve('buffer'), 'Buffer'],
            })
        );

        config.resolve.alias = {
            'proton-mail': path.resolve(__dirname, 'src/app/'),
            perf_hooks: path.resolve(__dirname, './perf_hooks_polyfill.ts'),
        };

        config.entry = mergeEntry(config.entry, {
            ['eo-index']: [path.resolve('./src/app/eo.tsx'), getSupportedEntry()],
        });

        config.devServer.historyApiFallback.rewrites = [{ from: /^\/eo/, to: '/eo.html' }];

        const htmlPlugin = config.plugins.find((plugin): plugin is HtmlWebpackPlugin => {
            return plugin instanceof HtmlWebpackPlugin;
        });
        if (!htmlPlugin) {
            throw new Error('Missing html plugin');
        }
        const htmlIndex = config.plugins.indexOf(htmlPlugin);

        if (webpackOptions.appMode === 'standalone') {
            addDevEntry(config);
        }

        config.plugins.splice(htmlIndex, 1, new HtmlWebpackPlugin({
                filename: 'index.html',
                template: path.resolve('./src/app.ejs'),
                templateParameters: htmlPlugin.userOptions.templateParameters,
                scriptLoading: 'defer',
                chunks: getIndexChunks('index'),
                inject: 'body',
            })
        );
        config.plugins.splice(
            htmlIndex,
            0,
            new HtmlWebpackPlugin({
                filename: 'eo.html',
                template: path.resolve('./src/eo.ejs'),
                templateParameters: htmlPlugin.userOptions.templateParameters,
                scriptLoading: 'defer',
                chunks: getIndexChunks('eo-index'),
                inject: 'body',
            })
        );

        config.experiments = { asyncWebAssembly: true };
    });
};

export default result;
