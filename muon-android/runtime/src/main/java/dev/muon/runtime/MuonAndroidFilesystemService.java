/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.os.Handler;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;

import androidx.annotation.NonNull;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

/** Performs cancellable real-path filesystem operations outside the UI thread. */
final class MuonAndroidFilesystemService implements AutoCloseable {
    private interface FilesystemOperation {
        @NonNull OperationResult run() throws Exception;
    }

    private static final class OperationResult {
        private static final int VOID = 0;
        private static final int STRING = 1;
        private static final int BOOLEAN = 2;
        private static final int BINARY = 3;

        final int kind;
        final String stringValue;
        final boolean booleanValue;
        final byte[] binaryValue;

        private OperationResult(
                int kind,
                String stringValue,
                boolean booleanValue,
                byte[] binaryValue) {
            this.kind = kind;
            this.stringValue = stringValue;
            this.booleanValue = booleanValue;
            this.binaryValue = binaryValue;
        }

        @NonNull static OperationResult voidResult() {
            return new OperationResult(VOID, null, false, null);
        }

        @NonNull static OperationResult stringResult(@NonNull String value) {
            return new OperationResult(STRING, value, false, null);
        }

        @NonNull static OperationResult booleanResult(boolean value) {
            return new OperationResult(BOOLEAN, null, value, null);
        }

        @NonNull static OperationResult binaryResult(@NonNull byte[] value) {
            return new OperationResult(BINARY, null, false, value);
        }

        void deliver(@NonNull MuonAndroidPlatformService.Completion completion) {
            switch (kind) {
                case VOID:
                    completion.completeVoid();
                    return;
                case STRING:
                    completion.completeString(stringValue);
                    return;
                case BOOLEAN:
                    completion.completeBoolean(booleanValue);
                    return;
                case BINARY:
                    completion.completeBinary(binaryValue);
                    return;
                default:
                    completion.fail("Android filesystem result kind is invalid");
            }
        }
    }

    private static final int MAXIMUM_READ_BYTES = 64 * 1024 * 1024;
    private static final int MAXIMUM_WATCHES = 16;
    private static final int COPY_BUFFER_BYTES = 64 * 1024;
    private static final int CREATE_FILE_MODE = 0666;
    private static final int CREATE_DIRECTORY_MODE = 0777;

    private final Handler mainHandler;
    private final ExecutorService executor = Executors.newFixedThreadPool(2);
    private final Map<Integer, Future<?>> pendingOperations = new TreeMap<>();
    private final Set<String> watchLeases = new HashSet<>();
    private boolean closed;

    MuonAndroidFilesystemService(@NonNull Handler mainHandler) {
        this.mainHandler = mainHandler;
    }

    void invoke(
            int callId,
            @NonNull String functionPath,
            @NonNull JSONArray arguments,
            @NonNull byte[][] attachments,
            @NonNull MuonAndroidPlatformService.Completion completion) {
        if (closed) {
            completion.fail("Android filesystem service was released");
            return;
        }
        FilesystemOperation operation;
        try {
            operation = createOperation(functionPath, arguments, attachments);
        } catch (Exception error) {
            completion.fail(formatDiagnostic(functionPath, error));
            return;
        }

        Future<?> future = executor.submit(() -> {
            OperationResult result = null;
            String diagnostic = null;
            try {
                result = operation.run();
            } catch (Exception error) {
                diagnostic = formatDiagnostic(functionPath, error);
            }
            OperationResult completedResult = result;
            String completedDiagnostic = diagnostic;
            mainHandler.post(() -> {
                if (pendingOperations.remove(callId) == null || closed) {
                    return;
                }
                if (completedDiagnostic == null) {
                    completedResult.deliver(completion);
                } else {
                    completion.fail(completedDiagnostic);
                }
            });
        });
        Future<?> previous = pendingOperations.put(callId, future);
        if (previous != null) {
            previous.cancel(true);
            future.cancel(true);
            pendingOperations.remove(callId);
            completion.fail("Duplicate Android filesystem call id");
        }
    }

    void cancel(int callId) {
        Future<?> future = pendingOperations.remove(callId);
        if (future != null) {
            future.cancel(true);
        }
    }

    void cancelAll() {
        for (Future<?> future : pendingOperations.values()) {
            future.cancel(true);
        }
        pendingOperations.clear();
        synchronized (watchLeases) {
            watchLeases.clear();
        }
    }

    int getActiveWatchCount() {
        synchronized (watchLeases) {
            return watchLeases.size();
        }
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        cancelAll();
        executor.shutdownNow();
    }

    @NonNull private FilesystemOperation createOperation(
            @NonNull String functionPath,
            @NonNull JSONArray arguments,
            @NonNull byte[][] attachments) throws Exception {
        switch (functionPath) {
            case "muon.fs.readFile": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                JSONObject options = requireObject(arguments, 1, "options");
                return () -> OperationResult.binaryResult(readFile(path, options));
            }
            case "muon.fs.writeFile": {
                requireArgumentCount(arguments, 3);
                byte[] data = requireBinary(arguments, 1, attachments);
                String path = requirePath(arguments, 0, "path");
                JSONObject options = requireObject(arguments, 2, "options");
                return () -> {
                    writeFile(path, data, options);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.readTextFile": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                requireUtf8Encoding(arguments, 1);
                return () -> OperationResult.stringResult(readTextFile(path));
            }
            case "muon.fs.writeTextFile": {
                requireArgumentCount(arguments, 3);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                String data = requireString(arguments, 1, "data");
                requireUtf8Encoding(arguments, 2);
                return () -> {
                    writeTextFile(path, data, false);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.stat":
            case "muon.fs.lstat": {
                requireArgumentCount(arguments, 1);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                boolean followLinks = functionPath.endsWith(".stat");
                return () -> OperationResult.stringResult(
                        createStats(path, followLinks).toString());
            }
            case "muon.fs.exists": {
                requireArgumentCount(arguments, 1);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                return () -> OperationResult.booleanResult(pathExists(path));
            }
            case "muon.fs.access": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                JSONObject options = requireObject(arguments, 1, "options");
                return () -> OperationResult.booleanResult(checkAccess(path, options));
            }
            case "muon.fs.readdir": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                JSONObject options = requireObject(arguments, 1, "options");
                boolean withFileTypes = requireOptionalBoolean(
                        options, "withFileTypes", false);
                return () -> OperationResult.stringResult(
                        readDirectory(path, withFileTypes).toString());
            }
            case "muon.fs.mkdir": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                JSONObject options = requireObject(arguments, 1, "options");
                boolean recursive = requireOptionalBoolean(options, "recursive", false);
                return () -> {
                    makeDirectory(path, recursive);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.rm": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                JSONObject options = requireObject(arguments, 1, "options");
                boolean recursive = requireOptionalBoolean(options, "recursive", false);
                boolean force = requireOptionalBoolean(options, "force", false);
                return () -> {
                    removePath(path, recursive, force);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.unlink":
            case "muon.fs.rmdir": {
                requireArgumentCount(arguments, 1);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                boolean directory = functionPath.endsWith(".rmdir");
                return () -> {
                    removeSinglePath(path, directory);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.rename": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String oldPath = requirePath(arguments, 0, "oldPath");
                String newPath = requirePath(arguments, 1, "newPath");
                return () -> {
                    Os.rename(oldPath, newPath);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.copyFile": {
                requireArgumentCount(arguments, 3);
                requireAttachmentCount(attachments, 0);
                String source = requirePath(arguments, 0, "source");
                String destination = requirePath(arguments, 1, "destination");
                JSONObject options = requireObject(arguments, 2, "options");
                boolean overwrite = requireOptionalBoolean(options, "overwrite", true);
                return () -> {
                    copyFile(source, destination, overwrite);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.appendFile": {
                requireArgumentCount(arguments, 2);
                byte[] data = requireBinary(arguments, 1, attachments);
                String path = requirePath(arguments, 0, "path");
                return () -> {
                    appendFile(path, data);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.appendTextFile": {
                requireArgumentCount(arguments, 3);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                String data = requireString(arguments, 1, "data");
                requireUtf8Encoding(arguments, 2);
                return () -> {
                    writeTextFile(path, data, true);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.truncate": {
                requireArgumentCount(arguments, 2);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                JSONObject options = requireObject(arguments, 1, "options");
                long length = requireUnsignedLong(options, "length", true, 0);
                return () -> {
                    truncate(path, length);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.realpath": {
                requireArgumentCount(arguments, 1);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                return () -> OperationResult.stringResult(realpath(path));
            }
            case "muon.fs.readlink": {
                requireArgumentCount(arguments, 1);
                requireAttachmentCount(attachments, 0);
                String path = requirePath(arguments, 0, "path");
                return () -> OperationResult.stringResult(Os.readlink(path));
            }
            case "muon.fs.symlink": {
                requireArgumentCount(arguments, 3);
                requireAttachmentCount(attachments, 0);
                String target = requireString(arguments, 0, "target");
                validateNoNul(target, "target");
                String path = requirePath(arguments, 1, "path");
                String type = requireString(arguments, 2, "type");
                if ("junction".equals(type)) {
                    throw new IOException(
                            "junction symbolic links are unavailable on Android");
                }
                if (!"file".equals(type) && !"dir".equals(type)) {
                    throw new IOException("type must be file or dir on Android");
                }
                return () -> {
                    Os.symlink(target, path);
                    return OperationResult.voidResult();
                };
            }
            case "muon.fs.watch": {
                requireArgumentCount(arguments, 1);
                requireAttachmentCount(attachments, 0);
                JSONObject request = requireObject(arguments, 0, "request");
                String operation = requireString(request, "operation");
                if ("acquire".equals(operation)) {
                    return () -> OperationResult.stringResult(acquireWatch().toString());
                }
                String token = requireString(request, "token");
                if ("release".equals(operation)) {
                    return () -> OperationResult.stringResult(releaseWatch(token).toString());
                }
                if ("snapshot".equals(operation)) {
                    String path = requirePath(request, "path");
                    return () -> OperationResult.stringResult(
                            createWatchSnapshot(path, token).toString());
                }
                throw new IOException("Unsupported Android filesystem watch operation");
            }
            default:
                throw new IOException("Unknown Android filesystem function: " + functionPath);
        }
    }

    private static void requireArgumentCount(@NonNull JSONArray arguments, int expected)
            throws IOException {
        if (arguments.length() != expected) {
            throw new IOException("Filesystem argument count is invalid");
        }
    }

    private static void requireAttachmentCount(@NonNull byte[][] attachments, int expected)
            throws IOException {
        if (attachments.length != expected) {
            throw new IOException("Filesystem binary attachment count is invalid");
        }
    }

    @NonNull private static JSONObject requireObject(
            @NonNull JSONArray arguments,
            int index,
            @NonNull String name) throws JSONException, IOException {
        Object value = arguments.get(index);
        if (!(value instanceof JSONObject)) {
            throw new IOException(name + " must be an object");
        }
        return (JSONObject) value;
    }

    @NonNull private static String requireString(
            @NonNull JSONArray arguments,
            int index,
            @NonNull String name) throws JSONException, IOException {
        Object value = arguments.get(index);
        if (!(value instanceof String) || ((String) value).isEmpty()) {
            throw new IOException(name + " must be a non-empty string");
        }
        return (String) value;
    }

    @NonNull private static String requireString(
            @NonNull JSONObject object,
            @NonNull String name) throws JSONException, IOException {
        if (!object.has(name)) {
            throw new IOException(name + " is required");
        }
        Object value = object.get(name);
        if (!(value instanceof String) || ((String) value).isEmpty()) {
            throw new IOException(name + " must be a non-empty string");
        }
        return (String) value;
    }

    @NonNull private static String requirePath(
            @NonNull JSONArray arguments,
            int index,
            @NonNull String name) throws JSONException, IOException {
        return validatePath(requireString(arguments, index, name), name);
    }

    @NonNull private static String requirePath(
            @NonNull JSONObject object,
            @NonNull String name) throws JSONException, IOException {
        return validatePath(requireString(object, name), name);
    }

    @NonNull private static String validatePath(
            @NonNull String path,
            @NonNull String name) throws IOException {
        validateNoNul(path, name);
        if (path.regionMatches(true, 0, "content://", 0, "content://".length())) {
            throw new IOException(
                    "content:// is unavailable to Android muon.fs until document URI support is added");
        }
        return path;
    }

    private static void validateNoNul(@NonNull String value, @NonNull String name)
            throws IOException {
        if (value.indexOf('\0') >= 0) {
            throw new IOException(name + " must not contain NUL");
        }
    }

    private static void requireUtf8Encoding(@NonNull JSONArray arguments, int index)
            throws JSONException, IOException {
        String encoding = requireString(arguments, index, "encoding");
        if (!"utf8".equals(encoding) && !"utf-8".equals(encoding)) {
            throw new IOException("encoding must be utf8 or utf-8");
        }
    }

    @NonNull private static byte[] requireBinary(
            @NonNull JSONArray arguments,
            int index,
            @NonNull byte[][] attachments) throws JSONException, IOException {
        JSONObject descriptor = requireObject(arguments, index, "data");
        int attachment = descriptor.getInt("attachment");
        int byteLength = descriptor.getInt("byteLength");
        if (attachment < 0
                || attachment >= attachments.length
                || attachments[attachment] == null
                || attachments[attachment].length != byteLength
                || attachments.length != 1) {
            throw new IOException("Filesystem binary attachment is invalid");
        }
        return attachments[attachment];
    }

    private static boolean requireOptionalBoolean(
            @NonNull JSONObject options,
            @NonNull String name,
            boolean defaultValue) throws JSONException, IOException {
        if (!options.has(name)) {
            return defaultValue;
        }
        Object value = options.get(name);
        if (!(value instanceof Boolean)) {
            throw new IOException(name + " must be a boolean");
        }
        return (Boolean) value;
    }

    private static long requireUnsignedLong(
            @NonNull JSONObject options,
            @NonNull String name,
            boolean required,
            long defaultValue) throws JSONException, IOException {
        if (!options.has(name)) {
            if (required) {
                throw new IOException(name + " is required");
            }
            return defaultValue;
        }
        Object value = options.get(name);
        if (!(value instanceof Number)) {
            throw new IOException(name + " must be a non-negative safe integer");
        }
        double number = ((Number) value).doubleValue();
        long integer = ((Number) value).longValue();
        if (!Double.isFinite(number)
                || number != integer
                || integer < 0
                || number > 9007199254740991.0) {
            throw new IOException(name + " must be a non-negative safe integer");
        }
        return integer;
    }

    private static void checkCanceled() throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedIOException("Filesystem operation was canceled");
        }
    }

    @NonNull private static byte[] readFile(
            @NonNull String path,
            @NonNull JSONObject options) throws Exception {
        long position = requireUnsignedLong(options, "position", false, 0);
        boolean hasLength = options.has("length");
        long explicitLength = requireUnsignedLong(options, "length", false, 0);
        if (hasLength && explicitLength > MAXIMUM_READ_BYTES) {
            throw new IOException(
                    "readFile length exceeds the 67108864 byte Android limit");
        }
        return readFile(path, position, hasLength ? explicitLength : null, MAXIMUM_READ_BYTES);
    }

    @NonNull private static byte[] readFile(
            @NonNull String path,
            long position,
            Long explicitLength,
            int maximumBytes) throws Exception {
        FileDescriptor descriptor = Os.open(
                path, OsConstants.O_RDONLY | OsConstants.O_CLOEXEC, 0);
        try {
            StructStat stats = Os.fstat(descriptor);
            long remaining = Math.max(0, stats.st_size - Math.min(position, stats.st_size));
            long requested = explicitLength == null
                    ? remaining
                    : Math.min(explicitLength, remaining);
            if (explicitLength == null && requested > maximumBytes) {
                throw new IOException(
                        "Filesystem read exceeds the 67108864 byte Android limit");
            }
            byte[] result = new byte[(int) requested];
            int offset = 0;
            while (offset < result.length) {
                checkCanceled();
                int count = Os.pread(
                        descriptor,
                        result,
                        offset,
                        result.length - offset,
                        position + offset);
                if (count <= 0) {
                    break;
                }
                offset += count;
            }
            return offset == result.length ? result : Arrays.copyOf(result, offset);
        } finally {
            Os.close(descriptor);
        }
    }

    private static void writeFile(
            @NonNull String path,
            @NonNull byte[] data,
            @NonNull JSONObject options) throws Exception {
        boolean hasPosition = options.has("position");
        long position = requireUnsignedLong(options, "position", false, 0);
        int flags = OsConstants.O_WRONLY | OsConstants.O_CREAT | OsConstants.O_CLOEXEC;
        if (!hasPosition) {
            flags |= OsConstants.O_TRUNC;
        }
        FileDescriptor descriptor = Os.open(path, flags, CREATE_FILE_MODE);
        try {
            writeBytes(descriptor, data, hasPosition ? position : null);
        } finally {
            Os.close(descriptor);
        }
    }

    private static void writeBytes(
            @NonNull FileDescriptor descriptor,
            @NonNull byte[] data,
            Long position) throws Exception {
        int offset = 0;
        while (offset < data.length) {
            checkCanceled();
            int count = position == null
                    ? Os.write(descriptor, data, offset, data.length - offset)
                    : Os.pwrite(
                            descriptor,
                            data,
                            offset,
                            data.length - offset,
                            position + offset);
            if (count <= 0) {
                throw new IOException("Filesystem write made no progress");
            }
            offset += count;
        }
    }

    @NonNull private static String readTextFile(@NonNull String path) throws Exception {
        byte[] data = readFile(path, 0, null, MAXIMUM_READ_BYTES);
        for (byte value : data) {
            if (value == 0) {
                throw new IOException("Text file must not contain NUL bytes");
            }
        }
        try {
            return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(data))
                    .toString();
        } catch (CharacterCodingException error) {
            throw new IOException("Text file must contain valid UTF-8", error);
        }
    }

    private static void writeTextFile(
            @NonNull String path,
            @NonNull String data,
            boolean append) throws Exception {
        validateNoNul(data, "data");
        byte[] bytes;
        try {
            ByteBuffer encoded = StandardCharsets.UTF_8.newEncoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .encode(CharBuffer.wrap(data));
            bytes = new byte[encoded.remaining()];
            encoded.get(bytes);
        } catch (CharacterCodingException error) {
            throw new IOException("Text data must contain valid Unicode", error);
        }
        if (append) {
            appendFile(path, bytes);
        } else {
            writeFile(path, bytes, new JSONObject());
        }
    }

    @NonNull private static JSONObject createStats(
            @NonNull String path,
            boolean followLinks) throws ErrnoException, JSONException {
        return createStats(followLinks ? Os.stat(path) : Os.lstat(path));
    }

    @NonNull private static JSONObject createStats(@NonNull StructStat stats)
            throws JSONException {
        String type;
        if (OsConstants.S_ISREG(stats.st_mode)) {
            type = "file";
        } else if (OsConstants.S_ISDIR(stats.st_mode)) {
            type = "directory";
        } else if (OsConstants.S_ISLNK(stats.st_mode)) {
            type = "symlink";
        } else if (OsConstants.S_ISBLK(stats.st_mode)) {
            type = "blockDevice";
        } else if (OsConstants.S_ISCHR(stats.st_mode)) {
            type = "characterDevice";
        } else if (OsConstants.S_ISFIFO(stats.st_mode)) {
            type = "fifo";
        } else if (OsConstants.S_ISSOCK(stats.st_mode)) {
            type = "socket";
        } else {
            type = "other";
        }
        boolean readonly = (stats.st_mode
                & (OsConstants.S_IWUSR | OsConstants.S_IWGRP | OsConstants.S_IWOTH)) == 0;
        JSONObject result = new JSONObject();
        result.put("type", type);
        result.put("size", "file".equals(type) ? stats.st_size : 0);
        result.put("mtimeMs", stats.st_mtime * 1000.0);
        result.put("readonly", readonly);
        return result;
    }

    private static boolean pathExists(@NonNull String path) {
        try {
            Os.stat(path);
            return true;
        } catch (ErrnoException error) {
            return false;
        }
    }

    private static boolean checkAccess(
            @NonNull String path,
            @NonNull JSONObject options) throws Exception {
        int mode = OsConstants.F_OK;
        if (options.has("mode")) {
            Object value = options.get("mode");
            if (!(value instanceof JSONArray)) {
                throw new IOException("mode must be an array");
            }
            JSONArray modes = (JSONArray) value;
            for (int index = 0; index < modes.length(); index += 1) {
                String entry = requireString(modes, index, "mode");
                switch (entry) {
                    case "read":
                        mode |= OsConstants.R_OK;
                        break;
                    case "write":
                        mode |= OsConstants.W_OK;
                        break;
                    case "execute":
                        mode |= OsConstants.X_OK;
                        break;
                    default:
                        throw new IOException(
                                "mode entries must be read, write, or execute");
                }
            }
        }
        try {
            return Os.access(path, mode);
        } catch (ErrnoException error) {
            return false;
        }
    }

    @NonNull private static JSONArray readDirectory(
            @NonNull String path,
            boolean withFileTypes) throws Exception {
        StructStat root = Os.stat(path);
        if (!OsConstants.S_ISDIR(root.st_mode)) {
            throw new IOException("readdir path is not a directory");
        }
        String[] names = new File(path).list();
        if (names == null) {
            throw new IOException("Could not enumerate directory");
        }
        Arrays.sort(names);
        JSONArray result = new JSONArray();
        for (String name : names) {
            checkCanceled();
            if (!withFileTypes) {
                result.put(name);
            } else {
                JSONObject entry = createStats(
                        new File(path, name).getPath(), false);
                entry.put("name", name);
                result.put(entry);
            }
        }
        return result;
    }

    private static void makeDirectory(@NonNull String path, boolean recursive)
            throws Exception {
        if (!recursive) {
            Os.mkdir(path, CREATE_DIRECTORY_MODE);
            return;
        }
        File directory = new File(path);
        if (directory.mkdirs()) {
            return;
        }
        StructStat stats = Os.stat(path);
        if (!OsConstants.S_ISDIR(stats.st_mode)) {
            throw new IOException("mkdir path exists and is not a directory");
        }
    }

    private static void removePath(
            @NonNull String path,
            boolean recursive,
            boolean force) throws Exception {
        StructStat stats;
        try {
            stats = Os.lstat(path);
        } catch (ErrnoException error) {
            if (force && error.errno == OsConstants.ENOENT) {
                return;
            }
            throw error;
        }
        if (OsConstants.S_ISDIR(stats.st_mode) && !recursive) {
            throw new IOException("rm requires recursive=true for directories");
        }
        removeRecursively(path, stats);
    }

    private static void removeRecursively(
            @NonNull String path,
            @NonNull StructStat stats) throws Exception {
        checkCanceled();
        if (OsConstants.S_ISDIR(stats.st_mode)) {
            String[] names = new File(path).list();
            if (names == null) {
                throw new IOException("Could not enumerate directory for removal");
            }
            for (String name : names) {
                String child = new File(path, name).getPath();
                removeRecursively(child, Os.lstat(child));
            }
        }
        Os.remove(path);
    }

    private static void removeSinglePath(@NonNull String path, boolean directory)
            throws Exception {
        StructStat stats = Os.lstat(path);
        boolean isDirectory = OsConstants.S_ISDIR(stats.st_mode);
        if (directory != isDirectory) {
            throw new IOException(directory
                    ? "rmdir path is not a directory"
                    : "unlink path is a directory");
        }
        Os.remove(path);
    }

    private static void copyFile(
            @NonNull String source,
            @NonNull String destination,
            boolean overwrite) throws Exception {
        FileDescriptor sourceDescriptor = Os.open(
                source, OsConstants.O_RDONLY | OsConstants.O_CLOEXEC, 0);
        try {
            if (!OsConstants.S_ISREG(Os.fstat(sourceDescriptor).st_mode)) {
                throw new IOException("copyFile source is not a regular file");
            }
            int flags = OsConstants.O_WRONLY
                    | OsConstants.O_CREAT
                    | OsConstants.O_CLOEXEC
                    | (overwrite ? OsConstants.O_TRUNC : OsConstants.O_EXCL);
            FileDescriptor destinationDescriptor = Os.open(
                    destination, flags, CREATE_FILE_MODE);
            try {
                byte[] buffer = new byte[COPY_BUFFER_BYTES];
                while (true) {
                    checkCanceled();
                    int count = Os.read(sourceDescriptor, buffer, 0, buffer.length);
                    if (count <= 0) {
                        break;
                    }
                    writeBytes(
                            destinationDescriptor,
                            count == buffer.length ? buffer : Arrays.copyOf(buffer, count),
                            null);
                }
            } finally {
                Os.close(destinationDescriptor);
            }
        } finally {
            Os.close(sourceDescriptor);
        }
    }

    private static void appendFile(@NonNull String path, @NonNull byte[] data)
            throws Exception {
        FileDescriptor descriptor = Os.open(
                path,
                OsConstants.O_WRONLY
                        | OsConstants.O_CREAT
                        | OsConstants.O_APPEND
                        | OsConstants.O_CLOEXEC,
                CREATE_FILE_MODE);
        try {
            writeBytes(descriptor, data, null);
        } finally {
            Os.close(descriptor);
        }
    }

    private static void truncate(@NonNull String path, long length) throws Exception {
        FileDescriptor descriptor = Os.open(
                path, OsConstants.O_WRONLY | OsConstants.O_CLOEXEC, 0);
        try {
            Os.ftruncate(descriptor, length);
        } finally {
            Os.close(descriptor);
        }
    }

    @NonNull private static String realpath(@NonNull String path) throws Exception {
        Os.stat(path);
        return new File(path).getCanonicalPath();
    }

    @NonNull private JSONObject acquireWatch() throws Exception {
        synchronized (watchLeases) {
            if (watchLeases.size() >= MAXIMUM_WATCHES) {
                throw new IOException("Android filesystem watcher limit is 16");
            }
            String token = UUID.randomUUID().toString();
            watchLeases.add(token);
            JSONObject result = new JSONObject();
            result.put("token", token);
            return result;
        }
    }

    @NonNull private JSONObject releaseWatch(@NonNull String token) throws JSONException {
        boolean released;
        synchronized (watchLeases) {
            released = watchLeases.remove(token);
        }
        JSONObject result = new JSONObject();
        result.put("released", released);
        return result;
    }

    @NonNull private JSONObject createWatchSnapshot(
            @NonNull String path,
            @NonNull String token) throws Exception {
        synchronized (watchLeases) {
            if (!watchLeases.contains(token)) {
                throw new IOException("Android filesystem watcher lease is unavailable");
            }
        }
        StructStat rootStats = Os.lstat(path);
        JSONObject result = new JSONObject();
        result.put("root", createStats(rootStats));
        JSONArray entries = new JSONArray();
        if (OsConstants.S_ISDIR(rootStats.st_mode)) {
            String[] names = new File(path).list();
            if (names == null) {
                throw new IOException("Could not enumerate watched directory");
            }
            Arrays.sort(names);
            for (String name : names) {
                checkCanceled();
                JSONObject entry = createStats(
                        new File(path, name).getPath(), false);
                entry.put("name", name);
                entries.put(entry);
            }
        }
        result.put("entries", entries);
        return result;
    }

    @NonNull private static String formatDiagnostic(
            @NonNull String functionPath,
            @NonNull Exception error) {
        String message = error.getMessage();
        if (message == null || message.isEmpty()) {
            message = error.getClass().getSimpleName();
        }
        return functionPath + ": " + message;
    }
}
