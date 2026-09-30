  dstorage_etw.h
      Trimmed copy of the DirectStorage ETW header, reduced to the provider
      GUID and the event ids and payload layouts the dispatcher decodes. Event
      descriptors carry only the event id. Note this
      header is not shipped in the Microsoft.Direct3D.DirectStorage NuGet
      package, which carries only dstorage.h and dstorageerr.h, so it has to be
      vendored rather than restored.
      Copyright (c) Microsoft Corporation.
