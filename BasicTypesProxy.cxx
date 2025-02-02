#include "SRProxy/BasicTypesProxy.h"

#include "TFile.h"
#include "TFormLeafInfo.h"
#include "TTreeFormula.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <fstream>

namespace
{
  /// Helper for CheckEquals
  template<class T> bool AreEqual(const T& x, const T& y)
  {
    if constexpr(std::is_floating_point_v<T>){
      return x == y || (std::isnan(x) && std::isnan(y));
    }
    else{
      return x == y;
    }
  }
}

namespace caf
{
  std::vector<Restorer*> SRProxySystController::fRestorers;
  long long SRProxySystController::fGeneration = 0;

  std::set<std::string> SRBranchRegistry::fgBranches;

  //----------------------------------------------------------------------
  void SRBranchRegistry::Print(bool abbrev)
  {
    std::string prev;
    for(std::string b: fgBranches){
      if(abbrev){
        unsigned int cutto = 0;
        for(unsigned int i = 0; i < std::min(b.size(), prev.size()); ++i){
          if(b[i] != prev[i]) break;
          if(b[i] == '.') cutto = i;
        }
        prev = b;
        for(unsigned int i = 0; i < cutto; ++i) b[i] = ' ';
      }
      std::cout << b << std::endl;
    }
  }

  //----------------------------------------------------------------------
  void SRBranchRegistry::ToFile(const std::string& fname)
  {
    std::ofstream fout(fname);
    for(const std::string& b: fgBranches) fout << b << std::endl;
  }

  //----------------------------------------------------------------------
  CAFType GetCAFType(const TDirectory* dir, TTree* tr)
  {
    if(!dir && !tr) return kCopiedRecord;
    if(dir) return kFlatMultiTree;
    if(tr->GetNbranches() > 1) return kFlatSingleTree;
    return kNested;
  }

  //----------------------------------------------------------------------
  std::string StripSubscripts(const std::string& s)
  {
    std::string ret;
    ret.reserve(s.size());
    bool insub = false;
    for(char c: s){
      /**/ if(c == '[') insub = true;
      else if(c == ']') insub = false;
      else if(!insub) ret += c;
    }
    return ret;
  }

  //----------------------------------------------------------------------
  int NSubscripts(const std::string& name)
  {
    return std::count(name.begin(), name.end(), '[');
  }

  //----------------------------------------------------------------------
  const long kZero = 0;

  // Sigh. For multi-tree flatcafs, 'base' is being updated by the caller to
  // give the row in the tree. But for single-tree flatcafs, this field
  // signifies a starting position within the array for the current row, and
  // should always be zero (ignoring the caller) for top-level fields. Enforce
  // that here.
  const long& AdjustBase(const long& base, CAFType type, const std::string& name)
  {
    if(type == kFlatSingleTree && NSubscripts(name) == 0) return kZero;
    return base;
  }

  //----------------------------------------------------------------------
  template<class T> Proxy<T>::Proxy(TDirectory* d, TTree* tr, const std::string& name, const long& base, int offset)
    : fName(name), fType(GetCAFType(d, tr)),
      fLeaf(0), fTree(tr), fDir(d),
      fBase(AdjustBase(base, fType, fName)), fOffset(offset),
      fLeafInfo(0), fBranch(0), fTTF(0), fEntry(-1), fSubIdx(0)
  {
  }

  const long kDummyBase = -1;

  //----------------------------------------------------------------------
  template<class T> Proxy<T>::Proxy(const Proxy<T>& p)
    : fName("copy of "+p.fName), fType(kCopiedRecord),
      fLeaf(0), fTree(0), fDir(0),
      fBase(kDummyBase), fOffset(-1),
      fLeafInfo(0), fBranch(0), fTTF(0), fEntry(-1), fSubIdx(-1)
  {
    // Ensure that the value is evaluated and baked in in the parent object, so
    // that fTTF et al aren't re-evaluated in every single copy.
    fVal = p.GetValue();
  }

  //----------------------------------------------------------------------
  template<class T> Proxy<T>::Proxy(const Proxy&& p)
    : fName("move of "+p.fName), fType(kCopiedRecord),
      fLeaf(0), fTree(0), fDir(0),
      fBase(kDummyBase), fOffset(-1),
      fLeafInfo(0), fBranch(0), fTTF(0), fEntry(-1), fSubIdx(-1)
  {
    // Ensure that the value is evaluated and baked in in the parent object, so
    // that fTTF et al aren't re-evaluated in every single copy.
    fVal = p.GetValue();
  }

  //----------------------------------------------------------------------
  template<class T> Proxy<T>::~Proxy()
  {
    // The other pointers aren't ours
    delete fTTF;
  }

  //----------------------------------------------------------------------
  template<class T> T Proxy<T>::GetValue() const
  {
    switch(fType){
    case kNested: return GetValueNested();
    case kFlatMultiTree: return GetValueFlatMulti();
    case kFlatSingleTree: return GetValueFlatSingle();
    case kCopiedRecord: return (T)fVal;
    default: abort();
    }
  }

  //----------------------------------------------------------------------
  template<class T> T Proxy<T>::GetValueChecked() const
  {
    const T val = GetValue();

    if constexpr(std::is_floating_point_v<T>){
      if(isnan(val) || isinf(val)){
        std::cout << "SRProxy: Warning: " << fName << " = " << val;
        if(fTree && fTree->GetDirectory() && fTree->GetDirectory()->GetFile()){
          std::cout << " in entry " << fEntry << " of " << fTree->GetDirectory()->GetFile()->GetName();
        }
        std::cout << std::endl;
      }
    }

    return val;
  }

  //----------------------------------------------------------------------
  template<class T> void GetTypedValueWrapper(TLeaf* leaf, T& x, int subidx)
  {
    x = leaf->GetTypedValue<T>(subidx);
  }

  //----------------------------------------------------------------------
  void GetTypedValueWrapper(TLeaf* leaf, std::string& x, int subidx)
  {
    assert(subidx == 0); // Unused for flat trees at least
    x = (char*)leaf->GetValuePointer();
  }

  //----------------------------------------------------------------------
  template<class T> T Proxy<T>::GetValueFlatSingle() const
  {
    assert(fTree);

    // Valid cached or systematically-shifted value
    if(fEntry == fTree->GetReadEntry()) return (T)fVal;
    fEntry = fTree->GetReadEntry();

    assert(fTree);

    if(!fLeaf){
      const std::string sname = StripSubscripts(fName);
      fLeaf = fTree->GetLeaf(sname.c_str());
      if(!fLeaf){
        std::cout << std::endl << "BasicTypeProxy: Branch '" << sname
                  << "' not found in tree '" << fTree->GetName() << "'."
                  << std::endl;
        abort();
      }

      fBranch = fLeaf->GetBranch();

      if(fName.find("..idx") == std::string::npos &&
         fName.find("..length") == std::string::npos){
        SRBranchRegistry::AddBranch(sname);
      }
    }

    fBranch->GetEntry(fEntry);

    GetTypedValueWrapper(fLeaf, fVal, fBase+fOffset);

    return (T)fVal;
  }

  //----------------------------------------------------------------------
  template<class T> T Proxy<T>::GetValueFlatMulti() const
  {
    // Valid cached or systematically-shifted value
    if(fEntry == fBase+fOffset) return (T)fVal;
    fEntry = fBase+fOffset;

    assert(fTree);

    if(!fLeaf){
      const std::string sname = StripSubscripts(fName);
      fLeaf = fTree->GetLeaf(sname.c_str());
      if(!fLeaf){
        std::cout << std::endl << "BasicTypeProxy: Branch '" << sname
                  << "' not found in tree '" << fTree->GetName() << "'."
                  << std::endl;
        abort();
      }

      fBranch = fLeaf->GetBranch();

      if(fName.find("_idx") == std::string::npos &&
         fName.find("_length") == std::string::npos &&
         fName.find(".size()") == std::string::npos){ // specific to "nested"
        SRBranchRegistry::AddBranch(sname);
      }
    }

    fBranch->GetEntry(fBase+fOffset);

    GetTypedValueWrapper(fLeaf, fVal, 0);

    return (T)fVal;
  }

  template<class T> void EvalInstanceWrapper(TTreeFormula* ttf, T& x)
  {
    // TODO is this the safest way to cast?
    x = (T)ttf->EvalInstance(0);
  }

  void EvalInstanceWrapper(TTreeFormula* ttf, std::string& x)
  {
    x = ttf->EvalStringInstance(0);
  }

  //----------------------------------------------------------------------
  template<class T> T Proxy<T>::GetValueNested() const
  {
    assert(fTree);

    // Valid cached or systematically-shifted value
    if(fEntry == fTree->GetReadEntry()) return (T)fVal;
    fEntry = fTree->GetReadEntry();

    // First time calling, set up the branches etc
    if(!fTTF){
      SRBranchRegistry::AddBranch(fName);

      // Leaves are attached to the TTF, must keep it
      fTTF = new TTreeFormula(("TTFProxy-"+fName).c_str(), fName.c_str(), fTree);
      fLeafInfo = fTTF->GetLeafInfo(0); // Can fail (for a regular branch?)
      fLeaf = fTTF->GetLeaf(0);
      fBranch = fLeaf->GetBranch();

      if(!fLeaf || !fBranch){
        std::cout << "Couldn't find " << fName << " in tree. Abort."
                  << std::endl;

        abort();
      }

      // TODO - parsing the array indices out sucks - pass in as an int somehow
      const size_t open_idx = fName.find_first_of('[');
      // Do we have exactly one set of [] in the name?
      if(open_idx != std::string::npos && open_idx == fName.find_last_of('[')){
	const size_t close_idx = fName.find_first_of(']');

	std::string numPart = fName.substr(open_idx+1, close_idx-open_idx-1);
	fSubIdx = atoi(numPart.c_str());
      }
    }

    if(fLeafInfo){
      // Using TTreeFormula always works, and is sometimes necessary

      fTTF->GetNdata(); // for some reason this is necessary for fTTF to work
                        // in all cases.

      EvalInstanceWrapper(fTTF, fVal);
    }
    else{
      // But when this is possible the hope is it might be faster

      if(fBranch->GetReadEntry() != fEntry){
        fBranch->GetEntry(fEntry);
      }

      // This check is much quicker than what CheckIndex() does, which winds up
      // calling a TTF, but I can't figure out a safe way to automatically
      // elide that check.
      if(fSubIdx > fLeaf->GetLen()){
        std::cout << std::endl << fName << " out of range (" << fName << ".size() == " << fLeaf->GetLen() << "). Aborting." << std::endl;
        abort();
      }

      GetTypedValueWrapper(fLeaf, fVal, fSubIdx);
    }

    return (T)fVal;
  }

  //----------------------------------------------------------------------
  template<class T> Proxy<T>& Proxy<T>::operator=(T x)
  {
    if(SRProxySystController::InTransaction()) SRProxySystController::Backup(*this);
    fVal = x;

    switch(fType){
    case kNested:         fEntry = fTree->GetReadEntry(); break;
    case kFlatMultiTree:  fEntry = fBase+fOffset;         break;
    case kFlatSingleTree: fEntry = fTree->GetReadEntry(); break;
    case kCopiedRecord: break;
    default: abort();
    }

    return *this;
  }

  //----------------------------------------------------------------------
  template<class T> Proxy<T>& Proxy<T>::operator+=(T x)
  {
    if constexpr(!std::is_same_v<T, bool>){
      // Do it this way to re-use the systematics logic in operator=
      *this = T(GetValue() + x);
    }
    else{
      std::cout << "Proxy<bool>::operator+=() is meaningless" << std::endl;
      (void)x;
      abort();
    }

    return *this;
  }

  //----------------------------------------------------------------------
  template<class T> Proxy<T>& Proxy<T>::operator*=(T x)
  {
    if constexpr(std::is_same_v<T, std::string>){
      std::cout << "Proxy<std::string>::operator*=() is meaningless" << std::endl;
      (void)x;
      abort();
    }
    else if constexpr(std::is_same_v<T, bool>){
      std::cout << "Proxy<bool>::operator*=() is meaningless" << std::endl;
      (void)x;
      abort();
    }
    else{
      // Do it this way to re-use the systematics logic in operator=
      *this = T(GetValue() * x);
    }

    return *this;
  }

  //----------------------------------------------------------------------
  template<class T> void Proxy<T>::CheckEquals(const T& x) const
  {
    if(!AreEqual(GetValue(), x)){
      std::cout << fName << " differs: "
                << GetValue() << " vs " << x << std::endl;
    }
  }

  //----------------------------------------------------------------------
  ArrayVectorProxyBase::ArrayVectorProxyBase(TDirectory* d, TTree* tr,
                                             const std::string& name,
                                             bool isNestedContainer,
                                             const long& base, int offset)
    : fDir(d), fTree(tr),
      fName(name), fIsNestedContainer(isNestedContainer),
      fType(GetCAFType(d, tr)),
      fBase(base), fOffset(offset),
      fIdxP(0), fIdx(0)
  {
  }

  //----------------------------------------------------------------------
  ArrayVectorProxyBase::~ArrayVectorProxyBase()
  {
    delete fIdxP;
  }

  //----------------------------------------------------------------------
  void ArrayVectorProxyBase::EnsureIdxP() const
  {
    if(fIdxP) return;

    // Only used for flat trees. For single-tree, only needed for objects not
    // at top-level.
    if(fType == kFlatMultiTree ||
       (fType == kFlatSingleTree && NSubscripts(fName) > 0)){
      fIdxP = new Proxy<long long>(fDir, fTree, IndexField(), fBase, fOffset);
    }
  }

  //----------------------------------------------------------------------
  void ArrayVectorProxyBase::CheckIndex(size_t i, size_t size) const
  {
    // This is the only way to get good error messages. But it also seems like
    // the call to size() here is necessary in Nested mode to trigger some
    // side-effect within ROOT, otherwise we get some bogus index out-of-range
    // crashes.
    if(i >= size){
      std::cout << std::endl << fName << "[" << (signed)i << "] out of range (" << fName << ".size() == " << size << "). Aborting." << std::endl;
      abort();
    }
  }

  //----------------------------------------------------------------------
  std::string VectorProxyBase::NName() const
  {
    const size_t idx = fName.find_last_of('.');
    if (idx != std::string::npos)
      // foo.bar.baz -> foo.bar.nbaz
      return fName.substr(0, idx)+".n"+fName.substr(idx+1);
    else
      // maybe the CAF is structured so this branch is at top level.
      // then it should just be "n" + the branch name
      return "n" + fName;
  }

  //----------------------------------------------------------------------
  std::string VectorProxyBase::LengthField() const
  {
    if(fType == kFlatMultiTree) return fName+"_length";
    if(fType == kFlatSingleTree) return fName+"..length";

    // Counts exist, but with non-systematic names
    if(fName == "rec.me.trkkalman"  ) return "rec.me.nkalman";
    if(fName == "rec.me.trkdiscrete") return "rec.me.ndiscrete";
    if(fName == "rec.me.trkcosmic"  ) return "rec.me.ncosmic";
    if(fName == "rec.me.trkbpf"     ) return "rec.me.nbpf";

    // foo.bar.baz -> foo.bar.nbaz
    const std::string nname = NName();

    if(!fTree) return nname; // doesn't matter if leaf exists or not

    int olderr = gErrorIgnoreLevel;
    gErrorIgnoreLevel = 99999999;
    TTreeFormula ttf(("TTFProxySize-"+fName).c_str(), nname.c_str(), fTree);
    TString junks = nname.c_str();
    int junki;
    const int def = ttf.DefinedVariable(junks, junki);
    gErrorIgnoreLevel = olderr;

    if(def >= 0) return nname;

    // Otherwise fallback and warn (this is on the first time we're accessed)

    // Don't emit the same warning more than once
    static std::set<std::string> already;

    const std::string key = StripSubscripts(NName());
    if(already.count(key) == 0){
      already.insert(key);
      std::cout << std::endl;
      std::cout << "Warning: field '" << key << "' does not exist in file. "
                << "Falling back to '" << StripSubscripts(fSize->Name()) << "' which is less efficient. "
                << "Consider updating StandardRecord to include '" << key << "'." << std::endl;
      std::cout << std::endl;
    }

    // foo.bar.baz -> foo.bar.@baz.size()
    const size_t idx = fName.find_last_of('.');
    return fName.substr(0, idx+1)+"@"+fName.substr(idx+1)+".size()";
  }

  //----------------------------------------------------------------------
  std::string ArrayVectorProxyBase::IndexField() const
  {
    if(fType == kFlatMultiTree) return fName+"_idx";
    if(fType == kFlatSingleTree) return fName+"..idx";
    abort();
  }

  //----------------------------------------------------------------------
  std::string ArrayVectorProxyBase::Subscript(int i) const
  {
    // Only have to do the at() business for the nested case for subscripts
    // from the 3rd one on
    if(fType != kNested || NSubscripts(fName) < 2){
      return SubName()+"["+std::to_string(i)+"]";
    }

    const size_t idx = fName.find_last_of('.'); // for nested name == subname

    return fName.substr(0, idx)+".@"+fName.substr(idx+1)+".at("+std::to_string(i)+")";
  }

  //----------------------------------------------------------------------
  std::string ArrayVectorProxyBase::SubName() const
  {
    // Nested containers would have the same name for length and idx at each
    // level, which is bad, so their names are uniquified.
    if(fType == kFlatSingleTree && fIsNestedContainer)
      return fName+".elems";
    else
      return fName;
  }

  //----------------------------------------------------------------------
  bool ArrayVectorProxyBase::TreeHasLeaf(TTree* tr,
                                         const std::string& name) const
  {
    return tr->GetLeaf(name.c_str());
  }

  //----------------------------------------------------------------------
  TTree* ArrayVectorProxyBase::GetTreeForName() const
  {
    if(fType != kFlatMultiTree) return fTree; // all in the same tree

    const std::string sname = StripSubscripts(fName);
    TTree* tr = (TTree*)fDir->Get(sname.c_str());
    if(!tr){
      std::cout << "Couldn't find TTree " << sname
                << " in " << fDir->GetName() << std::endl;
      abort();
    }
    return tr;
  }

  //----------------------------------------------------------------------
  VectorProxyBase::VectorProxyBase(TDirectory* d, TTree* tr,
                                   const std::string& name,
                                   bool isNestedContainer,
                                   const long& base, int offset)
    : ArrayVectorProxyBase(d, tr, name, isNestedContainer, base, offset),
      fSize(0)
  {
  }

  //----------------------------------------------------------------------
  VectorProxyBase::~VectorProxyBase()
  {
    delete fSize;
  }

  //----------------------------------------------------------------------
  void VectorProxyBase::EnsureSizeExists() const
  {
    if(fSize) return;

    fSize = new Proxy<int>(fDir, fTree, LengthField(), fBase, fOffset);
  }

  //----------------------------------------------------------------------
  size_t VectorProxyBase::size() const
  {
    EnsureSizeExists();
    return *fSize;
  }

  //----------------------------------------------------------------------
  bool VectorProxyBase::empty() const
  {
    return size() == 0;
  }

  //----------------------------------------------------------------------
  void VectorProxyBase::resize(size_t i)
  {
    EnsureSizeExists();
    *fSize = i;
  }

  // Enumerate all the variants we expect
  template class Proxy<char>;
  template class Proxy<short>;
  template class Proxy<int>;
  template class Proxy<long>;
  template class Proxy<long long>;

  template class Proxy<unsigned char>;
  template class Proxy<unsigned short>;
  template class Proxy<unsigned int>;
  template class Proxy<unsigned long>;
  template class Proxy<unsigned long long>;

  template class Proxy<float>;
  template class Proxy<double>;
  template class Proxy<long double>;

  template class Proxy<bool>;

  template class Proxy<std::string>;

} // namespace
