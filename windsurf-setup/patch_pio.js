/**
 * PlatformIO IDE 离线补丁脚本
 * 
 * 问题: PlatformIO 扩展初始化时会联网检查/更新 PIO Core，
 *       在中国大陆 PlatformIO 服务器 (lb1.platformio.org) 被墙，
 *       导致 "Initializing PlatformIO Core..." 永久卡住。
 * 
 * 方案: 跳过联网检查，直接使用本地已安装的 PIO Core 状态。
 *       需要先通过 PlatformIO 官方安装器安装好 PIO Core。
 * 
 * 用法: node patch_pio.js [windsurf扩展目录]
 */

const fs = require('fs');
const path = require('path');

const userProfile = process.env.USERPROFILE || process.env.HOME;

// 查找 PlatformIO 扩展目录
function findPioExtDir(extBase) {
    if (!fs.existsSync(extBase)) {
        console.error(`扩展目录不存在: ${extBase}`);
        return null;
    }
    const dirs = fs.readdirSync(extBase).filter(d => 
        d.includes('platformio-ide') && !d.includes('davidgomes')
    );
    if (dirs.length === 0) {
        console.error('未找到 PlatformIO IDE 扩展');
        return null;
    }
    return path.join(extBase, dirs[0]);
}

// 生成 core-dump 状态文件
function ensureCoreDump() {
    const pioDir = path.join(userProfile, '.platformio');
    const penvBin = path.join(pioDir, 'penv', 'Scripts');
    const pioExe = path.join(penvBin, 'platformio.exe');
    
    if (!fs.existsSync(pioExe)) {
        console.error(`PlatformIO Core 未安装: ${pioExe} 不存在`);
        console.error('请先安装 PlatformIO Core: https://docs.platformio.org/en/latest/core/installation.html');
        return null;
    }
    
    const cacheDir = path.join(pioDir, '.cache', 'tmp');
    fs.mkdirSync(cacheDir, { recursive: true });
    
    const dumpFile = path.join(cacheDir, 'core-dump-42364.json');
    
    // 获取 Python 版本
    let pyVersion = '3.11.7';
    const pyExe = path.join(penvBin, 'python.exe');
    if (fs.existsSync(pyExe)) {
        try {
            const { execSync } = require('child_process');
            const ver = execSync(`"${pyExe}" --version`, { encoding: 'utf-8' }).trim();
            const m = ver.match(/Python (\d+\.\d+\.\d+)/);
            if (m) pyVersion = m[1];
        } catch (e) { /* use default */ }
    }

    // 获取 PIO Core 版本
    let coreVersion = '6.1.19';
    try {
        const { execSync } = require('child_process');
        const ver = execSync(`"${pioExe}" --version`, { encoding: 'utf-8' }).trim();
        const m = ver.match(/version (\d+\.\d+\.\d+)/);
        if (m) coreVersion = m[1];
    } catch (e) { /* use default */ }

    const state = {
        core_version: coreVersion,
        python_version: pyVersion,
        core_dir: pioDir,
        cache_dir: path.join(pioDir, '.cache'),
        penv_dir: path.join(pioDir, 'penv'),
        penv_bin_dir: penvBin,
        platformio_exe: pioExe,
        installer_version: '1.2.2',
        python_exe: pyExe,
        system: 'windows_amd64',
        is_develop_core: false
    };

    fs.writeFileSync(dumpFile, JSON.stringify(state, null, 2));
    console.log(`Core state 已写入: ${dumpFile}`);
    console.log(`  PIO Core: ${coreVersion}, Python: ${pyVersion}`);
    return dumpFile;
}

// 补丁扩展代码
function patchExtension(extDir) {
    const extJs = path.join(extDir, 'dist', 'extension.js');
    if (!fs.existsSync(extJs)) {
        console.error(`扩展入口文件不存在: ${extJs}`);
        return false;
    }

    let content = fs.readFileSync(extJs, 'utf-8');

    // 检查是否已经补丁过
    if (content.includes('PIO_OFFLINE_PATCH')) {
        console.log('扩展已经补丁过，跳过');
        return true;
    }

    const target = 'e.report({message:"Initializing PlatformIO Core..."});try{return!await t.check()}catch(e){}return!0';
    
    if (!content.includes(target)) {
        console.error('未找到目标代码，可能扩展版本不兼容');
        return false;
    }

    const patch = [
        'e.report({message:"Initializing PlatformIO Core..."});',
        'try{/*PIO_OFFLINE_PATCH*/',
        'const _fs=require("fs"),_path=require("path");',
        'const _home=process.env.USERPROFILE||process.env.HOME;',
        'const _dump=_path.join(_home,".platformio",".cache","tmp","core-dump-42364.json");',
        'const _coreState=JSON.parse(_fs.readFileSync(_dump,"utf-8"));',
        'a.core.setCoreState(_coreState);',
        'const _py3=_path.join(_coreState.core_dir,"python3");',
        'const _bin=_coreState.penv_bin_dir;',
        'process.env.PLATFORMIO_PATH=[_py3,_bin,process.env.PLATFORMIO_PATH||process.env.PATH].join(_path.delimiter);',
        'console.info("PIO Offline Patch: loaded core state v"+_coreState.core_version);',
        'return false',
        '}catch(_e){console.error("PIO patch error:",_e);try{return!await t.check()}catch(e){}return!0}'
    ].join('');

    content = content.replace(target, patch);
    fs.writeFileSync(extJs, content, 'utf-8');
    console.log(`扩展已补丁: ${extJs}`);
    return true;
}

// 主流程
function main() {
    console.log('=== PlatformIO IDE 离线补丁工具 ===\n');

    // 1. 生成 core-dump
    const dumpFile = ensureCoreDump();
    if (!dumpFile) process.exit(1);

    // 2. 查找并补丁扩展
    const extBase = process.argv[2] || path.join(userProfile, '.windsurf', 'extensions');
    const extDir = findPioExtDir(extBase);
    if (!extDir) process.exit(1);

    console.log(`扩展目录: ${extDir}`);
    
    if (!patchExtension(extDir)) process.exit(1);

    console.log('\n补丁完成! 请重启 Windsurf (Ctrl+Shift+P -> Reload Window)');
}

main();
