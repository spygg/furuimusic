// 福瑞音乐 - 更新包「浏览下载结果」辅助
//
// 车机（比亚迪等）常禁止应用直接拉起系统安装器，只允许在系统文件管理器里点 APK 安装。
// 这里把下载目录/APK 通过 FileProvider 转成 content:// URI，交给系统文件管理器打开，
// 用户即可在文件管理器里找到更新包并手动安装。
package cn.furui.music;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.util.Log;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;

public final class UpdateInstaller {
    private static final String TAG = "FuruiUpdate";

    private UpdateInstaller() {}

    private static Uri fileUri(Activity activity, File file) {
        String authority = activity.getPackageName() + ".qtprovider";
        return FileProvider.getUriForFile(activity, authority, file);
    }

    /**
     * 判断下载目录里的某个文件名是否属于 displayName 这个更新包（含系统生成的
     * "xxx (1).apk" / "xxx (2).apk" 历史副本）。只认「同名」或「主干+空格+(数字)+.apk」，
     * 避免误删用户自己放的其他文件、或不同平台的更新包。
     */
    private static boolean isSameUpdate(String name, String displayName) {
        if (name == null) return false;
        if (name.equals(displayName)) return true;
        int dot = displayName.lastIndexOf('.');
        String base = dot > 0 ? displayName.substring(0, dot) : displayName;
        if (!name.startsWith(base + " (") || !name.endsWith(".apk")) return false;
        String mid = name.substring(base.length() + 2, name.length() - 4);
        if (mid.length() == 0) return false;
        for (int i = 0; i < mid.length(); ++i)
            if (!Character.isDigit(mid.charAt(i))) return false;
        return true;
    }

    /**
     * 删除下载目录里同名的旧更新包（含 (1)(2) 历史副本），保证覆盖而非累积。
     * 文件名带版本号，同名即同一版本，覆盖是正确语义。
     * 幂等：没有同名文件时什么都不做；删除失败也不阻断本次复制。
     */
    private static void removeSameNamed(ContentResolver resolver, String displayName) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // 分区存储：MediaStore 里删记录即删文件。列全名候选项后在 Java 侧
                // 用 isSameUpdate 精确筛选——DISPLAY_NAME 精确匹配清不掉 (1)(2) 残留。
                int dot = displayName.lastIndexOf('.');
                String base = dot > 0 ? displayName.substring(0, dot) : displayName;
                String sel = MediaStore.Downloads.DISPLAY_NAME + " LIKE ?";
                Cursor c = resolver.query(MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                        new String[]{MediaStore.Downloads._ID, MediaStore.Downloads.DISPLAY_NAME},
                        sel, new String[]{base + "%"}, null);
                if (c != null) {
                    try {
                        int idCol = c.getColumnIndexOrThrow(MediaStore.Downloads._ID);
                        int nameCol = c.getColumnIndexOrThrow(MediaStore.Downloads.DISPLAY_NAME);
                        while (c.moveToNext()) {
                            String name = c.getString(nameCol);
                            if (!isSameUpdate(name, displayName)) continue;
                            long id = c.getLong(idCol);
                            try {
                                resolver.delete(ContentUris.withAppendedId(
                                        MediaStore.Downloads.EXTERNAL_CONTENT_URI, id), null, null);
                            } catch (Throwable ignored) {}
                        }
                    } finally {
                        c.close();
                    }
                }
            } else {
                // Android 9-：公共 Download 可直写，删掉同名文件及 (1)(2) 副本，
                // 并清掉 MediaStore.Files 里指向这些路径的旧记录。
                File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
                if (dir == null) return;
                File[] files = dir.listFiles();
                if (files != null) {
                    for (File f : files)
                        if (isSameUpdate(f.getName(), displayName)) {
                            //noinspection ResultOfMethodCallIgnored
                            f.delete();
                        }
                }
                String sel = MediaStore.MediaColumns.DATA + " LIKE ?";
                Cursor c = resolver.query(MediaStore.Files.getContentUri("external"),
                        new String[]{MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DATA},
                        sel, new String[]{"%" + baseName(displayName) + "%"}, null);
                if (c != null) {
                    try {
                        int idCol = c.getColumnIndexOrThrow(MediaStore.MediaColumns._ID);
                        int dataCol = c.getColumnIndexOrThrow(MediaStore.MediaColumns.DATA);
                        while (c.moveToNext()) {
                            String data = c.getString(dataCol);
                            if (data == null || !isSameUpdate(new File(data).getName(), displayName)) continue;
                            long id = c.getLong(idCol);
                            try {
                                resolver.delete(ContentUris.withAppendedId(
                                        MediaStore.Files.getContentUri("external"), id), null, null);
                            } catch (Throwable ignored) {}
                        }
                    } finally {
                        c.close();
                    }
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "removeSameNamed failed (non-fatal)", t);
        }
    }

    /** 回读 MediaStore 里该 URI 的实际 DISPLAY_NAME（可能被系统自动加了序号）。 */
    private static String queryDisplayName(ContentResolver resolver, Uri uri) {
        Cursor c = null;
        try {
            c = resolver.query(uri, new String[]{MediaStore.MediaColumns.DISPLAY_NAME},
                               null, null, null);
            if (c != null && c.moveToFirst()) {
                int col = c.getColumnIndexOrThrow(MediaStore.MediaColumns.DISPLAY_NAME);
                return c.getString(col);
            }
        } catch (Throwable ignored) {
        } finally {
            if (c != null) c.close();
        }
        return null;
    }

    private static String baseName(String displayName) {
        int dot = displayName.lastIndexOf('.');
        return dot > 0 ? displayName.substring(0, dot) : displayName;
    }

    /**
     * 把应用私有目录里的更新 APK 复制到公共「下载」目录，返回文件展示名。
     * 车机（比亚迪 DiLink 等）禁止应用内拉起安装器，但系统文件管理器可以打开公共
     * Download 里的 APK 手动安装。私有 files/update/ 目录文件管理器看不到，所以
     * 安装失败时必须把包复制出来。
     *
     * targetSdk 35 + Android 10+ 分区存储：不能直写 /sdcard/Download，必须经
     * MediaStore.Downloads 插入；Android 9 及以下走传统 File 拷贝。
     * 返回空串表示失败（无存储权限/空间不足等）。
     */
    public static String copyToDownloads(Activity activity, String apkPath, String displayName) {
        if (activity == null || apkPath == null || displayName == null) return "";
        File src = new File(apkPath);
        if (!src.exists() || !src.isFile()) return "";
        try {
            ContentResolver resolver = activity.getContentResolver();
            // 安全覆盖：先用临时名写入，写成功后再删旧包并改名。这样写入失败
            // （磁盘满/权限）时旧的同名包仍完好，用户不会失去可安装的那一版。
            final String tmpName = ".furui_" + System.currentTimeMillis() + ".part";
            Uri target;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // Android 10+：分区存储，用 MediaStore 写公共 Download，无需存储权限
                ContentValues v = new ContentValues();
                v.put(MediaStore.Downloads.DISPLAY_NAME, tmpName);
                v.put(MediaStore.Downloads.MIME_TYPE, "application/vnd.android.package-archive");
                v.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
                v.put(MediaStore.Downloads.IS_PENDING, 1);
                target = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
                if (target == null) return "";
            } else {
                // Android 9 及以下：分区存储未启用，直接写公共 Download
                File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
                if (dir == null) return "";
                //noinspection ResultOfMethodCallIgnored
                dir.mkdirs();
                File dest = new File(dir, tmpName);
                ContentValues v = new ContentValues();
                v.put(MediaStore.MediaColumns.DISPLAY_NAME, tmpName);
                v.put(MediaStore.MediaColumns.MIME_TYPE, "application/vnd.android.package-archive");
                v.put(MediaStore.MediaColumns.DATA, dest.getAbsolutePath());
                target = resolver.insert(MediaStore.Files.getContentUri("external"), v);
                if (target == null) return "";
            }

        boolean copied = false;
        InputStream in = null;
        OutputStream out = null;
        try {
            in = new FileInputStream(src);
            out = resolver.openOutputStream(target);
            if (out == null) {
                Log.w(TAG, "copyToDownloads: openOutputStream returned null");
            } else {
                byte[] buf = new byte[256 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                copied = true;
            }
        } catch (Throwable writeErr) {
            Log.w(TAG, "copyToDownloads write failed", writeErr);
        } finally {
            // 必须先关闭输出流再复位 IS_PENDING / 删除回滚，否则 MediaStore 可能
            // 仍持有未 flush 的句柄，出现空文件或删不掉。
            if (out != null) try { out.close(); } catch (Throwable ignored) {}
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
        // 任何失败路径（含 out==null）都走这里回滚，不残留 IS_PENDING=1 幽灵记录
        if (!copied) {
            try { resolver.delete(target, null, null); } catch (Throwable ignored) {}
            return "";
        }
        // 写成功后才删旧包并改用正式名：写入失败时旧包完好，不会丢失可安装版本
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ContentValues done = new ContentValues();
            done.put(MediaStore.Downloads.IS_PENDING, 0);
            // 复位失败（部分车机 ROM）：文件停在 pending 对文件管理器不可见，
            // 视为失败并删除，让上层如实提示，而不是报成功却找不到文件。
            if (resolver.update(target, done, null, null) == 0) {
                Log.w(TAG, "copyToDownloads: reset IS_PENDING failed");
                try { resolver.delete(target, null, null); } catch (Throwable ignored) {}
                return "";
            }
            // 旧同名包（含历史 (1)(2) 副本）现在才删，新包已写成功，不会丢包
            removeSameNamed(resolver, displayName);
            ContentValues rename = new ContentValues();
            rename.put(MediaStore.Downloads.DISPLAY_NAME, displayName);
            if (resolver.update(target, rename, null, null) == 0) {
                // 改名失败的极端情况：文件仍在下载目录，只是叫临时名。
                // 回读真实名字返回，用户仍能在文件管理器里找到并安装。
                String tmpActual = queryDisplayName(resolver, target);
                Log.w(TAG, "copyToDownloads: rename failed, keep tmp name " + tmpActual);
                return tmpActual != null ? tmpActual : "";
            }
            String actual = queryDisplayName(resolver, target);
            Log.i(TAG, "copyToDownloads ok: " + (actual != null ? actual : displayName) + " -> " + target);
            return actual != null ? actual : displayName;
        }
        // Android 9-：先删旧同名包（此时新包还叫 tmpName，不会被误删），再把新包
        // 从临时名改为正式名，最后同步 MediaStore.Files 记录路径。
        File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        File dest = dir != null ? new File(dir, displayName) : null;
        File tmp = dir != null ? new File(dir, tmpName) : null;
        if (dest == null || tmp == null || !tmp.exists()) {
            Log.w(TAG, "copyToDownloads: tmp file missing");
            try { resolver.delete(target, null, null); } catch (Throwable ignored) {}
            return "";
        }
        removeSameNamed(resolver, displayName);
        if (!tmp.renameTo(dest)) {
            Log.w(TAG, "copyToDownloads: rename tmp failed");
            try { resolver.delete(target, null, null); } catch (Throwable ignored) {}
            return "";
        }
        try {
            ContentValues fix = new ContentValues();
            fix.put(MediaStore.MediaColumns.DISPLAY_NAME, displayName);
            fix.put(MediaStore.MediaColumns.DATA, dest.getAbsolutePath());
            resolver.update(target, fix, null, null);
        } catch (Throwable ignored) {}
        Log.i(TAG, "copyToDownloads ok: " + displayName + " -> " + dest.getAbsolutePath());
        return displayName;
        } catch (Throwable t) {
            Log.w(TAG, "copyToDownloads failed", t);
            return "";
        }
    }

    /**
     * 在系统文件管理器中打开更新包所在目录。返回是否成功拉起任意 Activity。
     */
    public static boolean openFolder(Activity activity, String apkPath) {
        if (activity == null || apkPath == null) return false;
        try {
            File apk = new File(apkPath);
            File dir = apk.getParentFile();
            if (dir == null || !dir.exists()) return false;
            Uri uri = fileUri(activity, dir);

            // ① 最常见：文件管理器认 resource/folder
            Intent i = new Intent(Intent.ACTION_VIEW);
            i.setDataAndType(uri, "resource/folder");
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            if (start(activity, i)) return true;

            // ② Android 文档 URI 的目录 MIME（API 24+）
            Intent i2 = new Intent(Intent.ACTION_VIEW);
            i2.setDataAndType(uri, DocumentsContract.Document.MIME_TYPE_DIR);
            i2.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            i2.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            if (start(activity, i2)) return true;

            // ③ 兜底：直接打开 APK 文件，让系统弹出「用哪个应用打开」（含文件管理器）
            Intent i3 = new Intent(Intent.ACTION_VIEW);
            i3.setDataAndType(fileUri(activity, apk), "*/*");
            i3.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            i3.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            // 用 chooser 强制弹出「用哪个应用打开」，让用户在车机上选系统文件管理器
            Intent chooser = Intent.createChooser(i3, "选择文件管理器打开更新包");
            chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            return start(activity, chooser);
        } catch (Throwable t) {
            Log.w(TAG, "openFolder failed", t);
            try {
                Toast.makeText(activity, "打开下载目录失败：" + t.getMessage(), Toast.LENGTH_LONG).show();
            } catch (Throwable ignored) {}
            return false;
        }
    }

    private static boolean start(Activity activity, Intent intent) {
        try {
            activity.startActivity(intent);
            return true;
        } catch (Throwable t) {
            return false;
        }
    }
}
