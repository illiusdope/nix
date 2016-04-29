#include "crypto.hh"
#include "globals.hh"
#include "store-api.hh"
#include "util.hh"
#include "nar-info-disk-cache.hh"


namespace nix {


bool isInStore(const Path & path)
{
    return isInDir(path, settings.nixStore);
}


bool isStorePath(const Path & path)
{
    return isInStore(path)
        && path.size() >= settings.nixStore.size() + 1 + storePathHashLen
        && path.find('/', settings.nixStore.size() + 1) == Path::npos;
}


void assertStorePath(const Path & path)
{
    if (!isStorePath(path))
        throw Error(format("path ‘%1%’ is not in the Nix store") % path);
}


Path toStorePath(const Path & path)
{
    if (!isInStore(path))
        throw Error(format("path ‘%1%’ is not in the Nix store") % path);
    Path::size_type slash = path.find('/', settings.nixStore.size() + 1);
    if (slash == Path::npos)
        return path;
    else
        return Path(path, 0, slash);
}


Path followLinksToStore(const Path & _path)
{
    Path path = absPath(_path);
    while (!isInStore(path)) {
        if (!isLink(path)) break;
        string target = readLink(path);
        path = absPath(target, dirOf(path));
    }
    if (!isInStore(path))
        throw Error(format("path ‘%1%’ is not in the Nix store") % path);
    return path;
}


Path followLinksToStorePath(const Path & path)
{
    return toStorePath(followLinksToStore(path));
}


string storePathToName(const Path & path)
{
    assertStorePath(path);
    auto l = settings.nixStore.size() + 1 + storePathHashLen;
    assert(path.size() >= l);
    return path.size() == l ? "" : string(path, l + 1);
}


string storePathToHash(const Path & path)
{
    assertStorePath(path);
    assert(path.size() >= settings.nixStore.size() + 1 + storePathHashLen);
    return string(path, settings.nixStore.size() + 1, storePathHashLen);
}


void checkStoreName(const string & name)
{
    string validChars = "+-._?=";
    /* Disallow names starting with a dot for possible security
       reasons (e.g., "." and ".."). */
    if (string(name, 0, 1) == ".")
        throw Error(format("illegal name: ‘%1%’") % name);
    for (auto & i : name)
        if (!((i >= 'A' && i <= 'Z') ||
              (i >= 'a' && i <= 'z') ||
              (i >= '0' && i <= '9') ||
              validChars.find(i) != string::npos))
        {
            throw Error(format("invalid character ‘%1%’ in name ‘%2%’")
                % i % name);
        }
}


/* Store paths have the following form:

   <store>/<h>-<name>

   where

   <store> = the location of the Nix store, usually /nix/store

   <name> = a human readable name for the path, typically obtained
     from the name attribute of the derivation, or the name of the
     source file from which the store path is created.  For derivation
     outputs other than the default "out" output, the string "-<id>"
     is suffixed to <name>.

   <h> = base-32 representation of the first 160 bits of a SHA-256
     hash of <s>; the hash part of the store name

   <s> = the string "<type>:sha256:<h2>:<store>:<name>";
     note that it includes the location of the store as well as the
     name to make sure that changes to either of those are reflected
     in the hash (e.g. you won't get /nix/store/<h>-name1 and
     /nix/store/<h>-name2 with equal hash parts).

   <type> = one of:
     "text:<r1>:<r2>:...<rN>"
       for plain text files written to the store using
       addTextToStore(); <r1> ... <rN> are the references of the
       path.
     "source"
       for paths copied to the store using addToStore() when recursive
       = true and hashAlgo = "sha256"
     "output:<id>"
       for either the outputs created by derivations, OR paths copied
       to the store using addToStore() with recursive != true or
       hashAlgo != "sha256" (in that case "source" is used; it's
       silly, but it's done that way for compatibility).  <id> is the
       name of the output (usually, "out").

   <h2> = base-16 representation of a SHA-256 hash of:
     if <type> = "text:...":
       the string written to the resulting store path
     if <type> = "source":
       the serialisation of the path from which this store path is
       copied, as returned by hashPath()
     if <type> = "output:<id>":
       for non-fixed derivation outputs:
         the derivation (see hashDerivationModulo() in
         primops.cc)
       for paths copied by addToStore() or produced by fixed-output
       derivations:
         the string "fixed:out:<rec><algo>:<hash>:", where
           <rec> = "r:" for recursive (path) hashes, or "" for flat
             (file) hashes
           <algo> = "md5", "sha1" or "sha256"
           <hash> = base-16 representation of the path or flat hash of
             the contents of the path (or expected contents of the
             path for fixed-output derivations)

   It would have been nicer to handle fixed-output derivations under
   "source", e.g. have something like "source:<rec><algo>", but we're
   stuck with this for now...

   The main reason for this way of computing names is to prevent name
   collisions (for security).  For instance, it shouldn't be feasible
   to come up with a derivation whose output path collides with the
   path for a copied source.  The former would have a <s> starting with
   "output:out:", while the latter would have a <2> starting with
   "source:".
*/


Path makeStorePath(const string & type,
    const Hash & hash, const string & name)
{
    /* e.g., "source:sha256:1abc...:/nix/store:foo.tar.gz" */
    string s = type + ":sha256:" + printHash(hash) + ":"
        + settings.nixStore + ":" + name;

    checkStoreName(name);

    return settings.nixStore + "/"
        + printHash32(compressHash(hashString(htSHA256, s), 20))
        + "-" + name;
}


Path makeOutputPath(const string & id,
    const Hash & hash, const string & name)
{
    return makeStorePath("output:" + id, hash,
        name + (id == "out" ? "" : "-" + id));
}


Path makeFixedOutputPath(bool recursive,
    HashType hashAlgo, Hash hash, string name)
{
    return hashAlgo == htSHA256 && recursive
        ? makeStorePath("source", hash, name)
        : makeStorePath("output:out", hashString(htSHA256,
                "fixed:out:" + (recursive ? (string) "r:" : "") +
                printHashType(hashAlgo) + ":" + printHash(hash) + ":"),
            name);
}


std::pair<Path, Hash> computeStorePathForPath(const Path & srcPath,
    bool recursive, HashType hashAlgo, PathFilter & filter)
{
    HashType ht(hashAlgo);
    Hash h = recursive ? hashPath(ht, srcPath, filter).first : hashFile(ht, srcPath);
    string name = baseNameOf(srcPath);
    Path dstPath = makeFixedOutputPath(recursive, hashAlgo, h, name);
    return std::pair<Path, Hash>(dstPath, h);
}


Path computeStorePathForText(const string & name, const string & s,
    const PathSet & references)
{
    Hash hash = hashString(htSHA256, s);
    /* Stuff the references (if any) into the type.  This is a bit
       hacky, but we can't put them in `s' since that would be
       ambiguous. */
    string type = "text";
    for (auto & i : references) {
        type += ":";
        type += i;
    }
    return makeStorePath(type, hash, name);
}


std::string Store::getUri()
{
    return "";
}


bool Store::isValidPath(const Path & storePath)
{
    auto hashPart = storePathToHash(storePath);

    {
        auto state_(state.lock());
        auto res = state_->pathInfoCache.get(hashPart);
        if (res) {
            stats.narInfoReadAverted++;
            return *res != 0;
        }
    }

    if (diskCache) {
        auto res = diskCache->lookupNarInfo(getUri(), hashPart);
        if (res.first != NarInfoDiskCache::oUnknown) {
            stats.narInfoReadAverted++;
            auto state_(state.lock());
            state_->pathInfoCache.upsert(hashPart,
                res.first == NarInfoDiskCache::oInvalid ? 0 : res.second);
            return res.first == NarInfoDiskCache::oValid;
        }
    }

    return isValidPathUncached(storePath);

    // FIXME: insert result into NARExistence table of diskCache.
}


ref<const ValidPathInfo> Store::queryPathInfo(const Path & storePath)
{
    auto hashPart = storePathToHash(storePath);

    {
        auto state_(state.lock());
        auto res = state_->pathInfoCache.get(hashPart);
        if (res) {
            stats.narInfoReadAverted++;
            if (!*res)
                throw InvalidPath(format("path ‘%s’ is not valid") % storePath);
            return ref<ValidPathInfo>(*res);
        }
    }

    if (diskCache) {
        auto res = diskCache->lookupNarInfo(getUri(), hashPart);
        if (res.first != NarInfoDiskCache::oUnknown) {
            stats.narInfoReadAverted++;
            auto state_(state.lock());
            state_->pathInfoCache.upsert(hashPart,
                res.first == NarInfoDiskCache::oInvalid ? 0 : res.second);
            if (res.first == NarInfoDiskCache::oInvalid ||
                (res.second->path != storePath && storePathToName(storePath) != ""))
                throw InvalidPath(format("path ‘%s’ is not valid") % storePath);
            return ref<ValidPathInfo>(res.second);
        }
    }

    auto info = queryPathInfoUncached(storePath);

    if (diskCache && info)
        diskCache->upsertNarInfo(getUri(), hashPart, info);

    {
        auto state_(state.lock());
        state_->pathInfoCache.upsert(hashPart, info);
    }

    if (!info
        || (info->path != storePath && storePathToName(storePath) != ""))
    {
        stats.narInfoMissing++;
        throw InvalidPath(format("path ‘%s’ is not valid") % storePath);
    }

    return ref<ValidPathInfo>(info);
}


/* Return a string accepted by decodeValidPathInfo() that
   registers the specified paths as valid.  Note: it's the
   responsibility of the caller to provide a closure. */
string Store::makeValidityRegistration(const PathSet & paths,
    bool showDerivers, bool showHash)
{
    string s = "";

    for (auto & i : paths) {
        s += i + "\n";

        auto info = queryPathInfo(i);

        if (showHash) {
            s += printHash(info->narHash) + "\n";
            s += (format("%1%\n") % info->narSize).str();
        }

        Path deriver = showDerivers ? info->deriver : "";
        s += deriver + "\n";

        s += (format("%1%\n") % info->references.size()).str();

        for (auto & j : info->references)
            s += j + "\n";
    }

    return s;
}


const Store::Stats & Store::getStats()
{
    {
        auto state_(state.lock());
        stats.pathInfoCacheSize = state_->pathInfoCache.size();
    }
    return stats;
}


ValidPathInfo decodeValidPathInfo(std::istream & str, bool hashGiven)
{
    ValidPathInfo info;
    getline(str, info.path);
    if (str.eof()) { info.path = ""; return info; }
    if (hashGiven) {
        string s;
        getline(str, s);
        info.narHash = parseHash(htSHA256, s);
        getline(str, s);
        if (!string2Int(s, info.narSize)) throw Error("number expected");
    }
    getline(str, info.deriver);
    string s; int n;
    getline(str, s);
    if (!string2Int(s, n)) throw Error("number expected");
    while (n--) {
        getline(str, s);
        info.references.insert(s);
    }
    if (!str || str.eof()) throw Error("missing input");
    return info;
}


string showPaths(const PathSet & paths)
{
    string s;
    for (auto & i : paths) {
        if (s.size() != 0) s += ", ";
        s += "‘" + i + "’";
    }
    return s;
}


void Store::exportPaths(const Paths & paths,
    bool sign, Sink & sink)
{
    for (auto & i : paths) {
        sink << 1;
        exportPath(i, sign, sink);
    }
    sink << 0;
}


std::string ValidPathInfo::fingerprint() const
{
    if (narSize == 0 || !narHash)
        throw Error(format("cannot calculate fingerprint of path ‘%s’ because its size/hash is not known")
            % path);
    return
        "1;" + path + ";"
        + printHashType(narHash.type) + ":" + printHash32(narHash) + ";"
        + std::to_string(narSize) + ";"
        + concatStringsSep(",", references);
}


void ValidPathInfo::sign(const SecretKey & secretKey)
{
    sigs.insert(secretKey.signDetached(fingerprint()));
}


unsigned int ValidPathInfo::checkSignatures(const PublicKeys & publicKeys) const
{
    unsigned int good = 0;
    for (auto & sig : sigs)
        if (checkSignature(publicKeys, sig))
            good++;
    return good;
}


bool ValidPathInfo::checkSignature(const PublicKeys & publicKeys, const std::string & sig) const
{
    return verifyDetached(fingerprint(), sig, publicKeys);
}


Strings ValidPathInfo::shortRefs() const
{
    Strings refs;
    for (auto & r : references)
        refs.push_back(baseNameOf(r));
    return refs;
}


}


#include "local-store.hh"
#include "remote-store.hh"


namespace nix {


RegisterStoreImplementation::Implementations * RegisterStoreImplementation::implementations = 0;


ref<Store> openStoreAt(const std::string & uri)
{
    for (auto fun : *RegisterStoreImplementation::implementations) {
        auto store = fun(uri);
        if (store) return ref<Store>(store);
    }

    throw Error(format("don't know how to open Nix store ‘%s’") % uri);
}


ref<Store> openStore()
{
    return openStoreAt(getEnv("NIX_REMOTE"));
}


static RegisterStoreImplementation regStore([](const std::string & uri) -> std::shared_ptr<Store> {
    enum { mDaemon, mLocal, mAuto } mode;

    if (uri == "daemon") mode = mDaemon;
    else if (uri == "local") mode = mLocal;
    else if (uri == "") mode = mAuto;
    else return 0;

    if (mode == mAuto) {
        if (LocalStore::haveWriteAccess())
            mode = mLocal;
        else if (pathExists(settings.nixDaemonSocketFile))
            mode = mDaemon;
        else
            mode = mLocal;
    }

    return mode == mDaemon
        ? std::shared_ptr<Store>(std::make_shared<RemoteStore>())
        : std::shared_ptr<Store>(std::make_shared<LocalStore>());
});


std::list<ref<Store>> getDefaultSubstituters()
{
    struct State {
        bool done = false;
        std::list<ref<Store>> stores;
    };
    static Sync<State> state_;

    auto state(state_.lock());

    if (state->done) return state->stores;

    StringSet done;

    auto addStore = [&](const std::string & uri) {
        if (done.count(uri)) return;
        done.insert(uri);
        state->stores.push_back(openStoreAt(uri));
    };

    for (auto uri : settings.get("substituters", Strings()))
        addStore(uri);

    for (auto uri : settings.get("binary-caches", Strings()))
        addStore(uri);

    for (auto uri : settings.get("extra-binary-caches", Strings()))
        addStore(uri);

    state->done = true;

    return state->stores;
}


}
