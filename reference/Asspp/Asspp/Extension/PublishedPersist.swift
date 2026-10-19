//
//  PublishedPersist.swift
//  NotchDrop
//
//  Created by 秋星桥 on 2024/7/8.
//

import Combine
import Foundation
import KeychainAccess

protocol PersistProvider {
    func data(forKey: String) -> Data?
    func set(_ data: Data?, forKey: String)
}

private let valueEncoder = JSONEncoder()
private let valueDecoder = JSONDecoder()
private let configDir = documentsDirectory
    .appendingPathComponent("Config")

/**
 We do not encrypt so we exclude these from backup.
 That means, no one else is able to access it if app is properly signed.
 */
class FileStorage: PersistProvider {
    func pathForKey(_ key: String) -> URL {
        try? FileManager.default.createDirectory(at: configDir, withIntermediateDirectories: true)
        var url = configDir.appendingPathComponent(key)
        var resourceValues = URLResourceValues()
        resourceValues.isExcludedFromBackup = true
        try? url.setResourceValues(resourceValues)
        return url
    }

    func data(forKey key: String) -> Data? {
        try? Data(contentsOf: pathForKey(key))
    }

    func set(_ data: Data?, forKey key: String) {
        // Atomic write so a crash mid-write cannot truncate persisted state
        // (download manifests, settings) into an undecodable blob.
        try? data?.write(to: pathForKey(key), options: .atomic)
    }
}

class KeychainStorage: PersistProvider {
    private let keychain: Keychain

    init(service: String) {
        keychain = Keychain(service: service).synchronizable(true)
    }

    func data(forKey key: String) -> Data? {
        try? keychain.getData(key)
    }

    func set(_ data: Data?, forKey key: String) {
        if let data {
            try? keychain.set(data, key: key)
        } else {
            try? keychain.remove(key)
        }
    }
}

@propertyWrapper
struct Persist<Value: Codable> {
    private let subject: CurrentValueSubject<Value, Never>
    private let cancellables: Set<AnyCancellable>
    private let engine: PersistProvider

    var projectedValue: AnyPublisher<Value, Never> {
        subject.eraseToAnyPublisher()
    }

    init(key: String, defaultValue: Value, engine: PersistProvider) {
        self.engine = engine
        if let data = engine.data(forKey: key) {
            if let object = try? valueDecoder.decode(Value.self, from: data) {
                subject = CurrentValueSubject<Value, Never>(object)
            } else {
                // Stored data exists but no longer decodes (shape change, partial
                // write). Back it up before the default value overwrites the
                // store so accounts/downloads stay recoverable instead of lost.
                engine.set(data, forKey: key + ".corrupt")
                logger.error("failed to decode persisted value for \(key); backed up to \(key).corrupt")
                subject = CurrentValueSubject<Value, Never>(defaultValue)
            }
        } else {
            subject = CurrentValueSubject<Value, Never>(defaultValue)
        }

        // Encode on the mutating thread so reference-type values cannot be
        // mutated mid-encode, then serialize the writes so rapid updates land
        // in order.
        let persistQueue = DispatchQueue(label: "wiki.qaq.persist.\(key)")
        var cancellables: Set<AnyCancellable> = .init()
        subject
            .map { try? valueEncoder.encode($0) }
            .removeDuplicates()
            .receive(on: persistQueue)
            .sink { engine.set($0, forKey: key) }
            .store(in: &cancellables)
        self.cancellables = cancellables
    }

    var wrappedValue: Value {
        get { subject.value }
        set { subject.send(newValue) }
    }

    func save() {
        subject.send(subject.value)
    }
}

extension PublishedPersist {
    init(key: String, defaultValue: Value, keychain: String) {
        let keychainStorage = KeychainStorage(service: keychain)
        self.init(key: key, defaultValue: defaultValue, engine: keychainStorage)
    }
}

@propertyWrapper
struct PublishedPersist<Value: Codable> {
    @Persist private var value: Value

    var projectedValue: AnyPublisher<Value, Never> {
        $value
    }

    @available(*, unavailable, message: "accessing wrappedValue will result undefined behavior")
    var wrappedValue: Value {
        get { value }
        set { value = newValue }
    }

    static subscript<EnclosingSelf: ObservableObject>(
        _enclosingInstance object: EnclosingSelf,
        wrapped _: ReferenceWritableKeyPath<EnclosingSelf, Value>,
        storage storageKeyPath: ReferenceWritableKeyPath<EnclosingSelf, PublishedPersist<Value>>,
    ) -> Value {
        get { object[keyPath: storageKeyPath].value }
        set {
            (object.objectWillChange as? ObservableObjectPublisher)?.send()
            object[keyPath: storageKeyPath].value = newValue
        }
    }

    init(key: String, defaultValue: Value, engine: PersistProvider) {
        _value = .init(key: key, defaultValue: defaultValue, engine: engine)
    }

    func save() {
        _value.save()
    }
}

extension Persist {
    init(key: String, defaultValue: Value) {
        self.init(key: key, defaultValue: defaultValue, engine: FileStorage())
    }
}

extension PublishedPersist {
    init(key: String, defaultValue: Value) {
        self.init(key: key, defaultValue: defaultValue, engine: FileStorage())
    }
}
