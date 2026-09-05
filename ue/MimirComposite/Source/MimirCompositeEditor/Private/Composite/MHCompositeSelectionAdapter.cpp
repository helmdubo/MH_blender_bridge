#include "Composite/MHCompositeSelectionAdapter.h"

#include "Elements/Framework/TypedElementSelectionSet.h"

namespace UE::MimirComposite
{

// R5b-2b red stub: no selection customization is registered.
bool MHRegisterPoolInstanceSelection(UTypedElementSelectionSet& SelectionSet)
{
    static_cast<void>(SelectionSet);
    return false;
}

bool MHIsPoolInstanceSelectionRegistered(const UTypedElementSelectionSet& SelectionSet)
{
    static_cast<void>(SelectionSet);
    return false;
}

} // namespace UE::MimirComposite
