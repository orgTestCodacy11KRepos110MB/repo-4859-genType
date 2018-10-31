open GenTypeCommon;

type env = {
  requiresEarly: ModuleNameMap.t((ImportPath.t, bool)),
  requires: ModuleNameMap.t((ImportPath.t, bool)),
  /* For each .cmt we import types from, keep the map of exported types. */
  cmtToExportTypeMap: StringMap.t(CodeItem.exportTypeMap),
  /* Map of types imported from other files. */
  exportTypeMapFromOtherFiles: CodeItem.exportTypeMap,
};

let requireModule = (~early, ~env, ~importPath, ~strict=false, moduleName) => {
  let requires = early ? env.requiresEarly : env.requires;
  let requiresNew =
    requires
    |> ModuleNameMap.add(
         moduleName,
         (
           moduleName |> ModuleResolver.resolveSourceModule(~importPath),
           strict,
         ),
       );
  early ?
    {...env, requiresEarly: requiresNew} : {...env, requires: requiresNew};
};

let createExportTypeMap =
    (~config, declarations: list(CodeItem.typeDeclaration))
    : CodeItem.exportTypeMap => {
  let updateExportTypeMap =
      (
        exportTypeMap: CodeItem.exportTypeMap,
        typeDeclaration: CodeItem.typeDeclaration,
      )
      : CodeItem.exportTypeMap => {
    let addExportType =
        (
          ~importTypes,
          ~annotation,
          {resolvedTypeName, typeVars, optTyp, _}: CodeItem.exportType,
        ) => {
      if (Debug.codeItems^) {
        logItem(
          "Type Map: %s%s%s\n",
          resolvedTypeName,
          typeVars == [] ?
            "" : "(" ++ (typeVars |> String.concat(",")) ++ ")",
          switch (optTyp) {
          | Some(typ) =>
            " "
            ++ (annotation |> Annotation.toString |> EmitText.comment)
            ++ " = "
            ++ (
              typ
              |> EmitTyp.typToString(~config, ~typeNameIsInterface=_ => false)
            )
          | None => ""
          },
        );
      };
      switch (optTyp) {
      | Some(typ) =>
        exportTypeMap
        |> StringMap.add(
             resolvedTypeName,
             {CodeItem.typeVars, typ, annotation, importTypes},
           )
      | None => exportTypeMap
      };
    };
    switch (typeDeclaration.exportFromTypeDeclaration) {
    | {exportKind: ExportType(exportType), annotation} =>
      exportType
      |> addExportType(~annotation, ~importTypes=typeDeclaration.importTypes)
    | {exportKind: ExportVariantLeaf(_) | ExportVariantType(_), _} => exportTypeMap
    };
  };
  declarations |> List.fold_left(updateExportTypeMap, StringMap.empty);
};

let codeItemToString = (~config, ~typeNameIsInterface, codeItem: CodeItem.t) =>
  switch (codeItem) {
  | ExportComponent({fileName, moduleName, _}) =>
    "ExportComponent fileName:"
    ++ (fileName |> ModuleName.toString)
    ++ " moduleName:"
    ++ (moduleName |> ModuleName.toString)
  | ExportValue({fileName, resolvedName, typ, _}) =>
    "WrapReasonValue"
    ++ " resolvedName:"
    ++ resolvedName
    ++ " moduleName:"
    ++ ModuleName.toString(fileName)
    ++ " typ:"
    ++ EmitTyp.typToString(~config, ~typeNameIsInterface, typ)
  | ImportComponent({importAnnotation, _}) =>
    "ImportComponent " ++ (importAnnotation.importPath |> ImportPath.toString)
  | ImportValue({importAnnotation, _}) =>
    "ImportValue " ++ (importAnnotation.importPath |> ImportPath.toString)
  };

let emitExportType =
    (
      ~early=?,
      ~emitters,
      ~config,
      ~typIsOpaque,
      ~typeNameIsInterface,
      {CodeItem.opaque, typeVars, resolvedTypeName, optTyp, _},
    ) => {
  let opaque =
    switch (opaque, optTyp) {
    | (Some(opaque), _) => opaque
    | (None, Some(typ)) => typ |> typIsOpaque
    | (None, None) => false
    };
  resolvedTypeName
  |> EmitTyp.emitExportType(
       ~early?,
       ~emitters,
       ~config,
       ~opaque,
       ~typeVars,
       ~optTyp,
       ~typeNameIsInterface,
     );
};

let typeNameIsInterface =
    (
      ~exportTypeMap: CodeItem.exportTypeMap,
      ~exportTypeMapFromOtherFiles: CodeItem.exportTypeMap,
      typeName,
    ) => {
  let typIsInterface = typ =>
    switch (typ) {
    | Object(_)
    | Record(_) => true
    | _ => false
    };
  switch (exportTypeMap |> StringMap.find(typeName)) {
  | {typ} => typ |> typIsInterface
  | exception Not_found =>
    switch (exportTypeMapFromOtherFiles |> StringMap.find(typeName)) {
    | {typ} => typ |> typIsInterface
    | exception Not_found => false
    }
  };
};

let emitexportFromTypeDeclaration =
    (
      ~config,
      ~emitters,
      ~typIsOpaque,
      ~env,
      ~typToConverter,
      ~enumTables,
      ~typeNameIsInterface,
      exportFromTypeDeclaration: CodeItem.exportFromTypeDeclaration,
    ) =>
  switch (exportFromTypeDeclaration.exportKind) {
  | ExportType(exportType) => (
      env,
      emitExportType(
        ~emitters,
        ~config,
        ~typIsOpaque,
        exportType,
        ~typeNameIsInterface,
      ),
    )

  | ExportVariantType({CodeItem.typeParams, variants, name, _}) => (
      env,
      EmitTyp.emitExportVariantType(
        ~emitters,
        ~config,
        ~name,
        ~typeNameIsInterface,
        ~typeParams,
        ~variants,
      ),
    )

  | ExportVariantLeaf({
      exportType,
      constructorTyp,
      argTypes,
      leafName,
      recordValue,
    }) =>
    let createFunctionType =
        ({CodeItem.typeVars, argTypes, variant: {name, params}}) => {
      let retType = Ident(name, params);
      if (argTypes === []) {
        retType;
      } else {
        Function({typeVars, argTypes, retType});
      };
    };

    let emitters =
      emitExportType(
        ~emitters,
        ~config,
        ~typIsOpaque,
        ~typeNameIsInterface,
        exportType,
      );

    let recordAsInt = recordValue |> Runtime.emitRecordAsInt(~config);
    let emitters =
      if (argTypes == []) {
        recordAsInt
        ++ ";"
        |> EmitTyp.emitExportConst(
             ~emitters,
             ~name=leafName,
             ~typeNameIsInterface,
             ~typ=constructorTyp |> createFunctionType,
             ~config,
           );
      } else {
        let args =
          argTypes
          |> List.mapi((i, typ) => {
               let converter = typ |> typToConverter;
               let arg = EmitText.argi(i + 1);
               let v = arg |> Converter.toReason(~converter, ~enumTables);
               (arg, v);
             });
        let mkReturn = s => "return " ++ s;
        let mkBody = args =>
          recordValue |> Runtime.emitRecordAsBlock(~config, ~args) |> mkReturn;
        EmitText.funDef(~args, ~mkBody, "")
        |> EmitTyp.emitExportConst(
             ~emitters,
             ~name=leafName,
             ~typeNameIsInterface,
             ~typ=constructorTyp |> createFunctionType,
             ~config,
           );
      };
    let env =
      ModuleName.createBucklescriptBlock
      |> requireModule(
           ~early=false,
           ~env,
           ~importPath=ImportPath.bsBlockPath(~config),
         );
    (env, emitters);
  };

let emitExportFromTypeDeclarations =
    (
      ~config,
      ~emitters,
      ~typIsOpaque,
      ~env,
      ~typToConverter,
      ~enumTables,
      ~typeNameIsInterface,
      exportFromTypeDeclarations,
    ) =>
  exportFromTypeDeclarations
  |> List.fold_left(
       ((env, emitters)) =>
         emitexportFromTypeDeclaration(
           ~config,
           ~emitters,
           ~typIsOpaque,
           ~env,
           ~typToConverter,
           ~typeNameIsInterface,
           ~enumTables,
         ),
       (env, emitters),
     );

let rec emitCodeItem =
        (
          ~config,
          ~outputFileRelative,
          ~resolver,
          ~emitters,
          ~env,
          ~enumTables,
          ~typIsOpaque,
          ~typToConverter,
          ~typeNameIsInterface,
          codeItem,
        ) => {
  let language = config.language;
  if (Debug.codeItems^) {
    logItem(
      "Code Item: %s\n",
      codeItem |> codeItemToString(~config, ~typeNameIsInterface),
    );
  };

  switch (codeItem) {
  | ImportComponent({
      exportType,
      importAnnotation,
      childrenTyp,
      propsFields,
      propsTypeName,
      fileName,
    }) =>
    let importPath = importAnnotation.importPath;
    let componentName = importAnnotation.name;

    let (emitters, env) =
      switch (language) {
      | TypeScript =>
        /* emit an import {... as ...} immediately */
        let emitters =
          importPath
          |> EmitTyp.emitImportValueAsEarly(
               ~emitters,
               ~name=componentName,
               ~nameAs=None,
             );
        (emitters, env);
      | Flow
      | Untyped =>
        /* add an early require(...)  */
        let env =
          componentName
          |> ModuleName.fromStringUnsafe
          |> requireModule(~early=true, ~env, ~importPath, ~strict=true);
        (emitters, env);
      };
    let componentNameTypeChecked = componentName ++ "TypeChecked";

    /* Check the type of the component */
    let emitters = EmitTyp.emitRequireReact(~early=true, ~emitters, ~config);
    let emitters =
      emitExportType(
        ~early=true,
        ~config,
        ~emitters,
        ~typIsOpaque,
        ~typeNameIsInterface,
        exportType,
      );
    let emitters =
      "("
      ++ (
        "props"
        |> EmitTyp.ofType(
             ~config,
             ~typeNameIsInterface,
             ~typ=Ident(propsTypeName, []),
           )
      )
      ++ ") {\n  return <"
      ++ componentName
      ++ " {...props}/>;\n}"
      |> EmitTyp.emitExportFunction(
           ~early=true,
           ~emitters,
           ~name=componentNameTypeChecked,
           ~config,
           ~comment=
             "In case of type error, check the type of '"
             ++ "make"
             ++ "' in '"
             ++ (fileName |> ModuleName.toString)
             ++ ".re'"
             ++ " and the props of '"
             ++ (importPath |> ImportPath.toString)
             ++ "'.",
         );

    /* Wrap the component */
    let emitters =
      (
        "function _"
        ++ EmitText.parens(
             (propsFields |> List.map(({name, _}: field) => name))
             @ ["children"]
             |> List.map(EmitTyp.ofTypeAny(~config)),
           )
        ++ " { return ReasonReact.wrapJsForReason"
        ++ EmitText.parens([
             componentName,
             "{"
             ++ (
               propsFields
               |> List.map(({name: propName, optional, typ: propTyp, _}) =>
                    propName
                    ++ ": "
                    ++ (
                      propName
                      |> Converter.toJS(
                           ~converter=
                             (
                               optional == Mandatory ?
                                 propTyp : Option(propTyp)
                             )
                             |> typToConverter,
                           ~enumTables,
                         )
                    )
                  )
               |> String.concat(", ")
             )
             ++ "}",
             "children"
             |> Converter.toJS(
                  ~converter=childrenTyp |> typToConverter,
                  ~enumTables,
                ),
           ])
        ++ "; }"
      )
      ++ ";"
      |> EmitTyp.emitExportConstEarly(
           ~comment=
             "Export '"
             ++ "make"
             ++ "' early to allow circular import from the '.bs.js' file.",
           ~emitters,
           ~name="make",
           ~typeNameIsInterface,
           ~typ=mixedOrUnknown(~config),
           ~config,
         );
    let env =
      ModuleName.reasonReact
      |> requireModule(
           ~early=true,
           ~env,
           ~importPath=ImportPath.reasonReactPath(~config),
         );
    (env, emitters);

  | ImportValue({valueName, importAnnotation, typ, fileName}) =>
    let importPath = importAnnotation.importPath;
    let (emitters, importedAsName, env) =
      switch (language) {
      | TypeScript =>
        /* emit an import {... as ...} immediately */
        let valueNameNotChecked = valueName ++ "NotChecked";
        let emitters =
          importPath
          |> EmitTyp.emitImportValueAsEarly(
               ~emitters,
               ~name=valueName,
               ~nameAs=Some(valueNameNotChecked),
             );
        (emitters, valueNameNotChecked, env);
      | Flow
      | Untyped =>
        /* add an early require(...)  */
        let importFile = importAnnotation.name;

        let importedAsName = importFile ++ "." ++ valueName;
        let env =
          importFile
          |> ModuleName.fromStringUnsafe
          |> requireModule(~early=true, ~env, ~importPath, ~strict=true);
        (emitters, importedAsName, env);
      };
    let converter = typ |> typToConverter;
    let valueNameTypeChecked = valueName ++ "TypeChecked";

    let emitters =
      importedAsName
      ++ ";"
      |> EmitTyp.emitExportConstEarly(
           ~emitters,
           ~name=valueNameTypeChecked,
           ~typeNameIsInterface,
           ~typ,
           ~config,
           ~comment=
             "In case of type error, check the type of '"
             ++ valueName
             ++ "' in '"
             ++ (fileName |> ModuleName.toString)
             ++ ".re'"
             ++ " and '"
             ++ (importPath |> ImportPath.toString)
             ++ "'.",
         );
    let emitters =
      (
        valueNameTypeChecked
        |> Converter.toReason(~converter, ~enumTables)
        |> EmitTyp.emitTypeCast(~config, ~typ, ~typeNameIsInterface)
      )
      ++ ";"
      |> EmitTyp.emitExportConstEarly(
           ~comment=
             "Export '"
             ++ valueName
             ++ "' early to allow circular import from the '.bs.js' file.",
           ~emitters,
           ~name=valueName,
           ~typeNameIsInterface,
           ~typ=mixedOrUnknown(~config),
           ~config,
         );
    (env, emitters);

  | ExportComponent({
      exportType,
      fileName,
      moduleName,
      propsTypeName,
      componentType,
      typ,
    }) =>
    let converter = typ |> typToConverter;
    let importPath =
      fileName
      |> ModuleResolver.resolveModule(
           ~config,
           ~outputFileRelative,
           ~resolver,
           ~importExtension=".bs",
         );
    let moduleNameBs = fileName |> ModuleName.forBsFile;

    let name = EmitTyp.componentExportName(~config, ~fileName, ~moduleName);
    let jsProps = "jsProps";
    let jsPropsDot = s => jsProps ++ "." ++ s;

    let args =
      switch (converter) {
      | FunctionC(groupedArgConverters, _retConverter) =>
        switch (groupedArgConverters) {
        | [
            GroupConverter(propConverters),
            ArgConverter(_, childrenConverter),
            ..._,
          ] =>
          (
            propConverters
            |> List.map(((s, argConverter)) =>
                 jsPropsDot(s)
                 |> Converter.toReason(~converter=argConverter, ~enumTables)
               )
          )
          @ [
            jsPropsDot("children")
            |> Converter.toReason(~converter=childrenConverter, ~enumTables),
          ]

        | [ArgConverter(_, childrenConverter), ..._] => [
            jsPropsDot("children")
            |> Converter.toReason(~converter=childrenConverter, ~enumTables),
          ]

        | _ => [jsPropsDot("children")]
        }

      | _ => [jsPropsDot("children")]
      };

    let emitters =
      emitExportType(
        ~emitters,
        ~config,
        ~typIsOpaque,
        ~typeNameIsInterface,
        exportType,
      );

    let numArgs = args |> List.length;
    let useCurry = numArgs >= 2;

    let emitCurry = (~args, name) =>
      switch (numArgs) {
      | 0
      | 1 => name ++ EmitText.parens(args)
      | (2 | 3 | 4 | 5 | 6 | 7 | 8) as n =>
        "Curry._" ++ (n |> string_of_int) ++ EmitText.parens([name] @ args)
      | _ => "Curry.app" ++ EmitText.parens([name, args |> EmitText.array])
      };

    let emitters =
      EmitTyp.emitExportConstMany(
        ~emitters,
        ~name,
        ~typeNameIsInterface,
        ~typ=componentType,
        ~config,
        [
          "ReasonReact.wrapReasonForJs(",
          "  " ++ ModuleName.toString(moduleNameBs) ++ ".component" ++ ",",
          "  (function _("
          ++ EmitTyp.ofType(
               ~config,
               ~typeNameIsInterface,
               ~typ=Ident(propsTypeName, []),
               jsProps,
             )
          ++ ") {",
          "     return "
          ++ (
            ModuleName.toString(moduleNameBs)
            ++ "."
            ++ "make"
            |> emitCurry(~args)
          )
          ++ ";",
          "  }));",
        ],
      );

    let emitters =
      /* only export default for the top level component in the file */
      fileName == moduleName ?
        EmitTyp.emitExportDefault(~emitters, ~config, name) : emitters;

    let env = moduleNameBs |> requireModule(~early=false, ~env, ~importPath);

    let env =
      ModuleName.reasonReact
      |> requireModule(
           ~early=false,
           ~env,
           ~importPath=ImportPath.reasonReactPath(~config),
         );

    let env =
      useCurry ?
        ModuleName.curry
        |> requireModule(
             ~early=false,
             ~env,
             ~importPath=ImportPath.bsCurryPath(~config),
           ) :
        env;

    (env, emitters);

  | ExportValue({fileName, resolvedName, valueAccessPath, typ}) =>
    let importPath =
      fileName
      |> ModuleResolver.resolveModule(
           ~config,
           ~outputFileRelative,
           ~resolver,
           ~importExtension=".bs",
         );
    let fileNameBs = fileName |> ModuleName.forBsFile;
    let envWithRequires =
      fileNameBs |> requireModule(~early=false, ~env, ~importPath);
    let converter = typ |> typToConverter;

    let emitters =
      (
        (fileNameBs |> ModuleName.toString)
        ++ "."
        ++ valueAccessPath
        |> Converter.toJS(~converter, ~enumTables)
      )
      ++ ";"
      |> EmitTyp.emitExportConst(
           ~emitters,
           ~name=resolvedName,
           ~typeNameIsInterface,
           ~typ,
           ~config,
         );

    (envWithRequires, emitters);
  };
}
and emitCodeItems =
    (
      ~config,
      ~outputFileRelative,
      ~resolver,
      ~emitters,
      ~env,
      ~enumTables,
      ~typIsOpaque,
      ~typToConverter,
      ~typeNameIsInterface,
      codeItems,
    ) =>
  codeItems
  |> List.fold_left(
       ((env, emitters)) =>
         emitCodeItem(
           ~config,
           ~outputFileRelative,
           ~resolver,
           ~emitters,
           ~env,
           ~enumTables,
           ~typIsOpaque,
           ~typToConverter,
           ~typeNameIsInterface,
         ),
       (env, emitters),
     );

let emitRequires = (~early, ~config, ~requires, emitters) =>
  ModuleNameMap.fold(
    (moduleName, (importPath, strict), emitters) =>
      importPath
      |> EmitTyp.emitRequire(~early, ~emitters, ~config, ~moduleName, ~strict),
    requires,
    emitters,
  );

let emitEnumTables = (~emitters, enumTables) => {
  let emitTable = (~hash, ~toJS, enum) =>
    "const "
    ++ hash
    ++ " = {"
    ++ (
      enum.cases
      |> List.map(label => {
           let js = label.labelJS |> EmitText.quotes;
           let re = label.label |> Runtime.emitVariantLabel(~comment=false);
           toJS ? (re |> EmitText.quotes) ++ ": " ++ js : js ++ ": " ++ re;
         })
      |> String.concat(", ")
    )
    ++ "};";
  Hashtbl.fold(
    (hash, (enum, toJS), emitters) =>
      enum |> emitTable(~hash, ~toJS) |> Emitters.import(~emitters),
    enumTables,
    emitters,
  );
};

let emitImportType =
    (
      ~config,
      ~outputFileRelative,
      ~resolver,
      ~emitters,
      ~inputCmtTranslateTypeDeclarations,
      ~typeNameIsInterface,
      ~env,
      {CodeItem.typeName, asTypeName, importPath, cmtFile},
    ) => {
  let (env, emitters) =
    switch (asTypeName, cmtFile) {
    | (None, _)
    | (_, None) => (env, emitters)
    | (Some(asType), Some(cmtFile)) =>
      let updateTypeMapFromOtherFiles = (~exportTypeMapFromCmt) =>
        switch (exportTypeMapFromCmt |> StringMap.find(typeName)) {
        | x => env.exportTypeMapFromOtherFiles |> StringMap.add(asType, x)
        | exception Not_found => exportTypeMapFromCmt
        };
      switch (env.cmtToExportTypeMap |> StringMap.find(cmtFile)) {
      | exportTypeMapFromCmt => (
          {
            ...env,
            exportTypeMapFromOtherFiles:
              updateTypeMapFromOtherFiles(~exportTypeMapFromCmt),
          },
          emitters,
        )
      | exception Not_found =>
        let exportTypeMapFromCmt =
          Cmt_format.read_cmt(cmtFile)
          |> inputCmtTranslateTypeDeclarations(
               ~config,
               ~outputFileRelative,
               ~resolver,
             )
          |> createExportTypeMap(~config);
        let cmtToExportTypeMap =
          env.cmtToExportTypeMap
          |> StringMap.add(cmtFile, exportTypeMapFromCmt);
        (
          {
            ...env,
            cmtToExportTypeMap,
            exportTypeMapFromOtherFiles:
              updateTypeMapFromOtherFiles(~exportTypeMapFromCmt),
          },
          emitters,
        );
      };
    };
  let emitters =
    EmitTyp.emitImportTypeAs(
      ~emitters,
      ~config,
      ~typeName,
      ~asTypeName,
      ~typeNameIsInterface=typeNameIsInterface(~env),
      ~importPath,
    );

  (env, emitters);
};

let emitImportTypes =
    (
      ~config,
      ~outputFileRelative,
      ~resolver,
      ~emitters,
      ~env,
      ~inputCmtTranslateTypeDeclarations,
      ~typeNameIsInterface,
      importTypes,
    ) =>
  importTypes
  |> List.fold_left(
       ((env, emitters)) =>
         emitImportType(
           ~config,
           ~outputFileRelative,
           ~resolver,
           ~emitters,
           ~inputCmtTranslateTypeDeclarations,
           ~typeNameIsInterface,
           ~env,
         ),
       (env, emitters),
     );

let getAnnotatedTypedDeclarations = (~annotatedSet, typeDeclarations) =>
  typeDeclarations
  |> List.map(typeDeclaration =>
       switch (typeDeclaration.CodeItem.exportFromTypeDeclaration.exportKind) {
       | ExportType(exportType) =>
         if (annotatedSet |> StringSet.mem(exportType.resolvedTypeName)) {
           {
             ...typeDeclaration,
             exportFromTypeDeclaration: {
               ...typeDeclaration.exportFromTypeDeclaration,
               annotation: GenType,
             },
           };
         } else {
           typeDeclaration;
         }
       | _ => typeDeclaration
       }
     )
  |> List.filter(
       (
         {exportFromTypeDeclaration: {annotation, _}, _}: CodeItem.typeDeclaration,
       ) =>
       annotation != NoGenType
     );

let propagateAnnotationToSubTypes = (typeMap: CodeItem.exportTypeMap) => {
  let annotatedSet = ref(StringSet.empty);
  let initialAnnotatedTypes =
    typeMap
    |> StringMap.bindings
    |> List.filter(((_, {CodeItem.annotation, _})) =>
         annotation == Annotation.GenType
       );
  let visitTypAndUpdateMarked = ((_typeName, {CodeItem.typ, annotation, _})) => {
    let visited = ref(StringSet.empty);
    let rec visit = typ =>
      switch (typ) {
      | Ident(typeName, _) =>
        if (visited^ |> StringSet.mem(typeName)) {
          ();
        } else {
          visited := visited^ |> StringSet.add(typeName);
          switch (typeMap |> StringMap.find(typeName)) {
          | {annotation: GenType | GenTypeOpaque | Generated, _} => ()
          | {typ: typ1, annotation: NoGenType, _} =>
            if (Debug.translation^) {
              logItem("Marking Type As Annotated %s\n", typeName);
            };
            annotatedSet := annotatedSet^ |> StringSet.add(typeName);
            typ1 |> visit;
          | exception Not_found => ()
          };
        }
      | Array(t, _) => t |> visit
      | Enum(_) => ()
      | Function({argTypes, retType, _}) =>
        argTypes |> List.iter(visit);
        retType |> visit;
      | GroupOfLabeledArgs(fields)
      | Object(fields)
      | Record(fields) => fields |> List.iter(({typ, _}) => typ |> visit)
      | Option(t)
      | Nullable(t) => t |> visit
      | Tuple(innerTypes) => innerTypes |> List.iter(visit)
      | TypeVar(_) => ()
      };
    switch ((annotation: Annotation.t)) {
    | GenType => typ |> visit
    | Generated
    | GenTypeOpaque
    | NoGenType => ()
    };
  };
  initialAnnotatedTypes |> List.iter(visitTypAndUpdateMarked);
  let newTypeMap =
    typeMap
    |> StringMap.mapi((typeName, exportTypeItem: CodeItem.exportTypeItem) =>
         {
           ...exportTypeItem,
           annotation:
             annotatedSet^ |> StringSet.mem(typeName) ?
               Annotation.GenType : exportTypeItem.annotation,
         }
       );

  (newTypeMap, annotatedSet^);
};

let emitTranslationAsString =
    (
      ~config,
      ~outputFileRelative,
      ~resolver,
      ~inputCmtTranslateTypeDeclarations,
      translation: Translation.t,
    ) => {
  let initialEnv = {
    requires: ModuleNameMap.empty,
    requiresEarly: ModuleNameMap.empty,
    cmtToExportTypeMap: StringMap.empty,
    exportTypeMapFromOtherFiles: StringMap.empty,
  };
  let enumTables = Hashtbl.create(1);

  let (exportTypeMap, annotatedSet) =
    translation.typeDeclarations
    |> createExportTypeMap(~config)
    |> propagateAnnotationToSubTypes;

  let annotatedTypeDeclarations =
    translation.typeDeclarations
    |> getAnnotatedTypedDeclarations(~annotatedSet);

  let importTypesFromTypeDeclarations =
    annotatedTypeDeclarations
    |> List.map((typeDeclaration: CodeItem.typeDeclaration) =>
         typeDeclaration.importTypes
       )
    |> List.concat;

  let exportFromTypeDeclarations =
    annotatedTypeDeclarations
    |> List.map((typeDeclaration: CodeItem.typeDeclaration) =>
         typeDeclaration.exportFromTypeDeclaration
       );

  let typeNameIsInterface = (~env) =>
    typeNameIsInterface(
      ~exportTypeMap,
      ~exportTypeMapFromOtherFiles=env.exportTypeMapFromOtherFiles,
    );

  let typIsOpaque_ = (~env, typ) =>
    typ
    |> Converter.typToConverterOpaque(
         ~config,
         ~exportTypeMap,
         ~exportTypeMapFromOtherFiles=env.exportTypeMapFromOtherFiles,
         ~typeNameIsInterface=typeNameIsInterface(~env),
       )
    |> snd;

  let typToConverter_ = (~env, typ) =>
    typ
    |> Converter.typToConverter(
         ~config,
         ~exportTypeMap,
         ~exportTypeMapFromOtherFiles=env.exportTypeMapFromOtherFiles,
         ~typeNameIsInterface=typeNameIsInterface(~env),
       );

  let emitters = Emitters.initial
  and env = initialEnv;

  let (env, emitters) =
    /* imports from type declarations go first to build up type tables */
    importTypesFromTypeDeclarations
    @ translation.importTypes
    |> List.sort_uniq(Translation.importTypeCompare)
    |> emitImportTypes(
         ~config,
         ~outputFileRelative,
         ~resolver,
         ~emitters,
         ~env,
         ~inputCmtTranslateTypeDeclarations,
         ~typeNameIsInterface,
       );

  let (env, emitters) =
    exportFromTypeDeclarations
    |> emitExportFromTypeDeclarations(
         ~config,
         ~emitters,
         ~typIsOpaque=typIsOpaque_(~env),
         ~env,
         ~typToConverter=typToConverter_(~env),
         ~enumTables,
         ~typeNameIsInterface=typeNameIsInterface(~env),
       );

  let (finalEnv, emitters) =
    translation.codeItems
    |> emitCodeItems(
         ~config,
         ~outputFileRelative,
         ~resolver,
         ~emitters,
         ~env,
         ~enumTables,
         ~typIsOpaque=typIsOpaque_(~env),
         ~typToConverter=typToConverter_(~env),
         ~typeNameIsInterface=typeNameIsInterface(~env),
       );

  let emitters = enumTables |> emitEnumTables(~emitters);

  emitters
  |> emitRequires(~early=true, ~config, ~requires=finalEnv.requiresEarly)
  |> emitRequires(~early=false, ~config, ~requires=finalEnv.requires)
  |> Emitters.toString(~separator="\n\n");
};