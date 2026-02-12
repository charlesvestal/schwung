const { notarize } = require('@electron/notarize');

exports.default = async function notarizing(context) {
    const { electronPlatformName, appOutDir } = context;
    if (electronPlatformName !== 'darwin') {
        return;
    }

    const appName = context.packager.appInfo.productFilename;
    const appPath = `${appOutDir}/${appName}.app`;

    console.log(`Notarizing ${appPath}...`);

    await notarize({
        appPath,
        tool: 'notarytool',
        keychainProfile: 'AC_PASSWORD',
    });

    console.log('Notarization complete.');
};
