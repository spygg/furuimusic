// 福瑞音乐 - 更新包「浏览下载结果」辅助
//
// 车机（比亚迪等）常禁止应用直接拉起系统安装器，只允许在系统文件管理器里点 APK 安装。
// 这里把下载目录/APK 通过 FileProvider 转成 content:// URI，交给系统文件管理器打开，
// 用户即可在文件管理器里找到更新包并手动安装。
package cn.furui.music;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import java.io.File;

public final class UpdateInstaller {
    private static final String TAG = "FuruiUpdate";

    private UpdateInstaller() {}

    private static Uri fileUri(Activity activity, File file) {
        String authority = activity.getPackageName() + ".qtprovider";
        return FileProvider.getUriForFile(activity, authority, file);
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
