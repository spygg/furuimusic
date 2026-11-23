// 福瑞音乐 - 车机兼容版专用：比亚迪小迪「新语音协议」AIDL 接口手写实现。
//
// 协议来自汽水车机版（com.luna.music.car）反编译的 com.bydsdk.autovoice 包：
//   IBydMediaAIDL      —— 车机语音服务(com.byd.autovoice)暴露的注册接口
//   IBydMediaListener  —— 我们实现，车机通过它下发 control/operation/query
//   IBydMediaCallback  —— 车机传入，我们用它回执执行结果
// 这里不生成 .aidl 交 apt 编译，而是手写 Parcel 事务，原因有二：
//   1) Qt 的 CMake/Android 打包链路不跑 aidl 步骤，手写最稳；
//   2) 反编译已给出精确 DESCRIPTOR 与事务号，照抄即可二进制兼容。
// 事务号/参数顺序必须逐字对齐，否则车机侧 enforceInterface 或 read 会失败。
package cn.furui.music;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;

import java.util.List;

public final class BydAidl {
    private BydAidl() {}

    // -------------------- IBydMediaAIDL（车机服务端接口，我们只调用） --------------------
    public interface IBydMediaAIDL extends IInterface {
        void register(String pkg, int[] mediaTypes, IBydMediaListener listener) throws RemoteException;
        void registerWithName(String pkg, String name, int[] mediaTypes, IBydMediaListener listener) throws RemoteException;
        void registerWithUid(int uid, String pkg, int[] mediaTypes, IBydMediaListener listener) throws RemoteException;
        void unregister(String pkg) throws RemoteException;

        abstract class Stub extends Binder implements IBydMediaAIDL {
            static final String DESCRIPTOR = "com.bydsdk.autovoice.IBydMediaAIDL";
            public Stub() { attachInterface(this, DESCRIPTOR); }
            public static IBydMediaAIDL asInterface(IBinder b) {
                if (b == null) return null;
                IInterface iin = b.queryLocalInterface(DESCRIPTOR);
                return (iin instanceof IBydMediaAIDL) ? (IBydMediaAIDL) iin : new Proxy(b);
            }
            @Override public IBinder asBinder() { return this; }
            @Override public boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code == 1) {
                    data.enforceInterface(DESCRIPTOR);
                    register(data.readString(), data.createIntArray(),
                             IBydMediaListener.Stub.asInterface(data.readStrongBinder()));
                    reply.writeNoException();
                    return true;
                }
                if (code == 2) {
                    data.enforceInterface(DESCRIPTOR);
                    registerWithName(data.readString(), data.readString(), data.createIntArray(),
                                     IBydMediaListener.Stub.asInterface(data.readStrongBinder()));
                    reply.writeNoException();
                    return true;
                }
                if (code == 3) {
                    data.enforceInterface(DESCRIPTOR);
                    registerWithUid(data.readInt(), data.readString(), data.createIntArray(),
                                    IBydMediaListener.Stub.asInterface(data.readStrongBinder()));
                    reply.writeNoException();
                    return true;
                }
                if (code == 4) {
                    data.enforceInterface(DESCRIPTOR);
                    unregister(data.readString());
                    reply.writeNoException();
                    return true;
                }
                if (code == 1598968902) { reply.writeString(DESCRIPTOR); return true; }
                return super.onTransact(code, data, reply, flags);
            }
            private static class Proxy implements IBydMediaAIDL {
                private final IBinder mRemote;
                Proxy(IBinder b) { mRemote = b; }
                public IBinder asBinder() { return mRemote; }
                public void register(String pkg, int[] types, IBydMediaListener l) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeString(pkg); d.writeIntArray(types);
                        d.writeStrongBinder(l != null ? l.asBinder() : null);
                        mRemote.transact(1, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
                public void registerWithName(String pkg, String name, int[] types, IBydMediaListener l) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeString(pkg); d.writeString(name); d.writeIntArray(types);
                        d.writeStrongBinder(l != null ? l.asBinder() : null);
                        mRemote.transact(2, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
                public void registerWithUid(int uid, String pkg, int[] types, IBydMediaListener l) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeInt(uid); d.writeString(pkg); d.writeIntArray(types);
                        d.writeStrongBinder(l != null ? l.asBinder() : null);
                        mRemote.transact(3, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
                public void unregister(String pkg) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeString(pkg);
                        mRemote.transact(4, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
            }
        }
    }

    // -------------------- IBydMediaCallback（车机传入，我们回执） --------------------
    public interface IBydMediaCallback extends IInterface {
        void onResult(String mediaApp, String json) throws RemoteException;

        abstract class Stub extends Binder implements IBydMediaCallback {
            static final String DESCRIPTOR = "com.bydsdk.autovoice.IBydMediaCallback";
            public Stub() { attachInterface(this, DESCRIPTOR); }
            public static IBydMediaCallback asInterface(IBinder b) {
                if (b == null) return null;
                IInterface iin = b.queryLocalInterface(DESCRIPTOR);
                return (iin instanceof IBydMediaCallback) ? (IBydMediaCallback) iin : new Proxy(b);
            }
            @Override public IBinder asBinder() { return this; }
            @Override public boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code == 1) {
                    data.enforceInterface(DESCRIPTOR);
                    onResult(data.readString(), data.readString());
                    reply.writeNoException();
                    return true;
                }
                if (code == 1598968902) { reply.writeString(DESCRIPTOR); return true; }
                return super.onTransact(code, data, reply, flags);
            }
            private static class Proxy implements IBydMediaCallback {
                private final IBinder mRemote;
                Proxy(IBinder b) { mRemote = b; }
                public IBinder asBinder() { return mRemote; }
                public void onResult(String mediaApp, String json) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeString(mediaApp); d.writeString(json);
                        mRemote.transact(1, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
            }
        }
    }

    // -------------------- IBydMediaListener（我们实现，车机下发命令） --------------------
    public interface IBydMediaListener extends IInterface {
        void setCallback(IBydMediaCallback cb) throws RemoteException;
        void control(String method, String json) throws RemoteException;
        void operation(String method, String json) throws RemoteException;
        void query(String method, String json) throws RemoteException;

        abstract class Stub extends Binder implements IBydMediaListener {
            static final String DESCRIPTOR = "com.bydsdk.autovoice.IBydMediaListener";
            public Stub() { attachInterface(this, DESCRIPTOR); }
            public static IBydMediaListener asInterface(IBinder b) {
                if (b == null) return null;
                IInterface iin = b.queryLocalInterface(DESCRIPTOR);
                return (iin instanceof IBydMediaListener) ? (IBydMediaListener) iin : new Proxy(b);
            }
            @Override public IBinder asBinder() { return this; }
            @Override public boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code == 1) {
                    data.enforceInterface(DESCRIPTOR);
                    setCallback(IBydMediaCallback.Stub.asInterface(data.readStrongBinder()));
                    reply.writeNoException(); return true;
                }
                if (code == 2) {
                    data.enforceInterface(DESCRIPTOR);
                    control(data.readString(), data.readString());
                    reply.writeNoException(); return true;
                }
                if (code == 3) {
                    data.enforceInterface(DESCRIPTOR);
                    operation(data.readString(), data.readString());
                    reply.writeNoException(); return true;
                }
                if (code == 4) {
                    data.enforceInterface(DESCRIPTOR);
                    query(data.readString(), data.readString());
                    reply.writeNoException(); return true;
                }
                if (code == 1598968902) { reply.writeString(DESCRIPTOR); return true; }
                return super.onTransact(code, data, reply, flags);
            }
            private static class Proxy implements IBydMediaListener {
                private final IBinder mRemote;
                Proxy(IBinder b) { mRemote = b; }
                public IBinder asBinder() { return mRemote; }
                public void setCallback(IBydMediaCallback cb) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeStrongBinder(cb != null ? cb.asBinder() : null);
                        mRemote.transact(1, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
                public void control(String method, String json) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeString(method); d.writeString(json);
                        mRemote.transact(2, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
                public void operation(String method, String json) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeString(method); d.writeString(json);
                        mRemote.transact(3, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
                public void query(String method, String json) throws RemoteException {
                    Parcel d = Parcel.obtain(), r = Parcel.obtain();
                    try {
                        d.writeInterfaceToken(DESCRIPTOR);
                        d.writeString(method); d.writeString(json);
                        mRemote.transact(4, d, r, 0); r.readException();
                    } finally { r.recycle(); d.recycle(); }
                }
            }
        }
    }
}
